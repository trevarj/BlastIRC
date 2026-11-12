#include "../src/application.hpp"
#include "../src/commands.hpp"
#include "../src/irc/protocol.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <poll.h>
#include <sqlite3.h>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace blast {
namespace {
// One bounded plaintext peer drives the real worker without a test-side thread.
struct RenameDrainPeer {
    using Clock = std::chrono::steady_clock;
    struct Fd {
        int value = -1;
        ~Fd() {
            if (value >= 0)
                ::close(value);
        }
    } listener, connection;
    string buffered;
    int port = 0;
    static void require(bool condition, const char *reason) {
        if (!condition)
            throw std::runtime_error(reason);
    }
    RenameDrainPeer() {
        listener.value = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        require(listener.value >= 0, "Cannot create rename-drain listener");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(::bind(listener.value, reinterpret_cast<sockaddr *>(&address), sizeof(address)) ==
                        0 &&
                    ::listen(listener.value, 1) == 0,
                "Cannot bind rename-drain listener");
        socklen_t size = sizeof(address);
        require(::getsockname(listener.value, reinterpret_cast<sockaddr *>(&address), &size) == 0,
                "Cannot read rename-drain port");
        port = ntohs(address.sin_port);
    }
    static void waitFd(int fd, short flags, Clock::time_point deadline) {
        for (;;) {
            require(Clock::now() < deadline, "Rename-drain socket deadline expired");
            const auto remaining =
                std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
            pollfd item{fd, flags, 0};
            const int result = ::poll(&item, 1, std::max(1, static_cast<int>(remaining.count())));
            if (result < 0 && errno == EINTR)
                continue;
            require(result > 0 && !(item.revents & POLLNVAL), "Rename-drain socket wait failed");
            return;
        }
    }
    void accept() {
        const auto deadline = Clock::now() + std::chrono::seconds(3);
        while (connection.value < 0) {
            waitFd(listener.value, POLLIN, deadline);
            connection.value =
                ::accept4(listener.value, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            require(connection.value >= 0 || errno == EINTR || errno == EAGAIN ||
                        errno == EWOULDBLOCK,
                    "Cannot accept rename-drain client");
        }
    }
    void send(const string &line) {
        const string frame = line + "\r\n";
        const auto deadline = Clock::now() + std::chrono::seconds(3);
        size_t offset = 0;
        while (offset < frame.size()) {
            waitFd(connection.value, POLLOUT, deadline);
            const auto result = ::send(connection.value, frame.data() + offset,
                                       frame.size() - offset, MSG_NOSIGNAL);
            if (result > 0)
                offset += static_cast<size_t>(result);
            else
                require(result < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK),
                        "Cannot send rename-drain frame");
        }
    }
    irc::WireMessage read() {
        const auto deadline = Clock::now() + std::chrono::seconds(3);
        for (;;) {
            const auto end = buffered.find("\r\n");
            if (end != string::npos) {
                auto message =
                    irc::parse(std::string_view(buffered).substr(0, end), irc::incomingCoreLimit);
                require(message.has_value(), "Invalid rename-drain client frame");
                buffered.erase(0, end + 2);
                return std::move(*message);
            }
            waitFd(connection.value, POLLIN, deadline);
            char bytes[4096];
            const auto result = ::recv(connection.value, bytes, sizeof(bytes), 0);
            if (result > 0) {
                buffered.append(bytes, static_cast<size_t>(result));
                require(buffered.size() < 65536, "Rename-drain client frame exceeds test bound");
            } else
                require(result < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK),
                        "Cannot read rename-drain client frame");
        }
    }
    void expect(std::string_view command, std::initializer_list<std::string_view> params) {
        const auto message = read();
        if (message.command == command &&
            std::equal(message.params.begin(), message.params.end(), params.begin(), params.end()))
            return;
        const auto describe = [](std::string_view name, const auto &values) {
            string text(name);
            text += " [";
            if (name != "CAP" && name != "NICK" && name != "USER" && name != "WHO" &&
                name != "PONG")
                return text + "parameters redacted]";
            for (const auto &value : values) {
                if (text.back() != '[')
                    text += ", ";
                text += '"' + string(value) + '"';
            }
            return text + "]";
        };
        throw std::runtime_error("Unexpected rename-drain client frame: expected " +
                                 describe(command, params) + ", received " +
                                 describe(message.command, message.params));
    }
    void fence() {
        send("PING :rename-drain-fence");
        expect("PONG", {"rename-drain-fence"});
    }
};
} // namespace
struct ApplicationRegression {
    static void completeOwnedHistory(Application &app, Buffer &room, Event event,
                                     const string &owner) {
        event.tags["history-owner"] = owner;
        Application::HistoryPage page;
        page.server = room.server;
        page.buffer = room.id;
        page.session = app.model.networks[room.server].sessionGeneration;
        page.request.target = room.target;
        page.request.requestId = owner;
        page.request.direction = HistoryDirection::Around;
        app.historyPages_[owner] = std::move(page);
        app.activeHistory_[owner] = room.id;
        const auto before = room.messages;
        const auto timestamp = room.lastChatTimestamp;
        const auto sequence = room.lastChatSequence;
        const auto effects = app.model.effects.size();
        app.reduce(event); // Submission labels remain independent of the enclosing page owner.
        assert(room.messages.size() == before.size() && room.lastChatTimestamp == timestamp &&
               room.lastChatSequence == sequence && app.model.effects.size() == effects);
        for (size_t i = 0; i < before.size(); ++i)
            assert(room.messages[i].id == before[i].id &&
                   room.messages[i].label == before[i].label &&
                   room.messages[i].timestamp == before[i].timestamp &&
                   room.messages[i].pending == before[i].pending &&
                   room.messages[i].sendStatus == before[i].sendStatus);
        assert(app.historyPages_.at(owner).events.size() == 1);
        Event end;
        end.type = "history_end";
        end.server = room.server;
        end.target = room.target;
        end.label = owner;
        end.tags = {
            {"history-owner", owner},   {"direction", "around"}, {"count", "1"},
            {"oldest-id", event.id},    {"newest-id", event.id}, {"oldest-time", event.time},
            {"newest-time", event.time}};
        const bool savingAllowed = app.savingAllowed_;
        app.savingAllowed_ = true;
        app.storage_.prune(app.model.settings);
        app.reduce(end);
        app.savingAllowed_ = savingAllowed;
        assert(!app.historyPages_.contains(owner));
    }

    static void repeatSearch() {
        Application app;
        app.model = Model{};
        app.savingAllowed_ = false;
        auto &first = app.buffer("repeat-one", "#source");
        auto &second = app.buffer("repeat-two", "#source");
        Message message;
        message.id = "same-message-id";
        message.nick = "Peer";
        message.reply = "parent-id";
        message.text = "first alpha beta";
        app.insert(first, message, false);
        message.text = "second alpha beta";
        app.insert(second, message, false);
        const string firstId = first.id, secondId = second.id;
        app.action({"search", firstId, "alpha", {}});
        auto &firstResults = app.buffer(first.server, "*search*", "search");
        assert(firstResults.searchSource == firstId && firstResults.messages.size() == 1);
        const auto searchRow = firstResults.messages.front();
        assert(searchRow.sourceBuffer == firstId && searchRow.sourceServer == first.server &&
               searchRow.sourceTarget == first.target && searchRow.sourceId == message.id &&
               searchRow.sourceText == "first alpha beta" &&
               searchRow.sourceReply == message.reply);
        first.draft = "draft survives navigation";
        const auto beforeJump = app.model.pendingSourceJump.revision;
        app.action({"search-jump", firstResults.id, searchRow.id, {}});
        assert(app.model.activeBuffer == firstId && first.draft == "draft survives navigation" &&
               app.model.pendingSourceJump.revision == beforeJump + 1 &&
               app.model.pendingSourceJump.buffer == firstId &&
               app.model.pendingSourceJump.message == message.id);
        app.model.activeBuffer = firstResults.id;
        firstResults.messages.front().sourceText = "forged payload";
        const auto beforeForged = app.model.pendingSourceJump.revision;
        app.action({"search-jump", firstResults.id, searchRow.id, {}});
        assert(app.model.activeBuffer == firstResults.id &&
               app.model.pendingSourceJump.revision == beforeForged);
        firstResults.messages.front() = searchRow;
        firstResults.searchSource.clear();
        app.model.activeBuffer = firstId;
        app.action({"search", firstResults.id, "no matching text", {}});
        assert(app.model.status == "Choose a conversation to search" &&
               firstResults.messages.size() == 1);
        firstResults.searchSource = firstId;
        app.action({"search", secondId, "alpha", {}});
        auto &secondResults = app.buffer(second.server, "*search*", "search");
        assert(secondResults.searchSource == secondId && secondResults.messages.size() == 1);
        // Dialogs capture the active result ID when the user reopens Search.
        app.action({"search", firstResults.id, "beta", {}});
        assert(app.model.activeBuffer == firstResults.id && firstResults.messages.size() == 1 &&
               firstResults.messages.front().text.find("first alpha beta") != string::npos &&
               secondResults.messages.front().text.find("second alpha beta") != string::npos);
        app.action({"search", firstResults.id, "no matching text", {}});
        assert(firstResults.messages.empty() && !firstResults.loading);
        app.renameBuffer(firstId, "#renamed");
        const string renamed = "repeat-one/#renamed";
        assert(firstResults.searchSource == renamed);
        app.action({"search", firstResults.id, "alpha", {}});
        assert(firstResults.messages.size() == 1 &&
               firstResults.messages.front().text.starts_with("[#renamed]"));
        app.clear(app.findBuffer(renamed));
        app.action({"search", firstResults.id, "alpha", {}});
        assert(firstResults.messages.empty() && firstResults.searchSource == renamed);
        app.model.buffers.erase(renamed);
        app.model.status.clear();
        app.action({"search", firstResults.id, "alpha", {}});
        assert(app.model.status == "Choose a conversation to search" &&
               firstResults.messages.empty() && secondResults.messages.size() == 1);
        firstResults.searchSource = secondId;
        app.action({"search", firstResults.id, "alpha", {}});
        assert(firstResults.messages.empty() && secondResults.messages.size() == 1);
    }
    static void retainedSearchJump() {
        Application app;
        app.model = Model{};
        app.model.settings.retentionDays = 0;
        auto &source = app.buffer("retained-search", "#source");
        source.draft = "retained draft";
        Message original;
        original.id = "retained-original";
        original.nick = "Peer";
        original.text = "retained unique search text";
        original.time = "2026-11-01T12:00:00Z";
        original.timestamp = 1793534400;
        app.storage_.append(source.id, original);
        assert(source.messages.empty());
        app.action({"search", source.id, "unique search", {}});
        auto &results = app.buffer(source.server, "*search*", "search");
        assert(results.messages.size() == 1 && source.messages.empty());
        const auto revision = app.model.pendingSourceJump.revision;
        app.action({"search-jump", results.id, results.messages.front().id, {}});
        assert(app.model.pendingSourceJump.revision == revision + 1 &&
               app.model.pendingSourceJump.buffer == source.id &&
               app.model.pendingSourceJump.message == original.id &&
               app.model.activeBuffer == source.id && source.draft == "retained draft" &&
               std::any_of(source.messages.begin(), source.messages.end(),
                           [&](const auto &m) { return m.id == original.id; }));
        app.action({"search", source.id, "unique search", {}});
        const string sourceId = source.id;
        const string resultId = results.id;
        const string rowId = results.messages.front().id;
        app.action({"close_buffer", sourceId, {}, {}});
        assert(!app.model.buffers.contains(sourceId));
        const auto closedRevision = app.model.pendingSourceJump.revision;
        app.action({"search-jump", resultId, rowId, {}});
        assert(app.model.buffers.contains(sourceId) && app.model.activeBuffer == sourceId &&
               app.model.pendingSourceJump.revision == closedRevision + 1 &&
               app.model.pendingSourceJump.buffer == sourceId &&
               app.model.pendingSourceJump.message == original.id &&
               std::any_of(app.model.buffers.at(sourceId).messages.begin(),
                           app.model.buffers.at(sourceId).messages.end(),
                           [&](const auto &m) { return m.id == original.id; }));
    }
    static void correlatedSearchHistory() {
        Application app;
        app.model = Model{};
        app.model.settings.retentionDays = 0;
        auto &source = app.buffer("search-history", "#source");
        source.draft = "pending draft";
        auto &results = app.buffer(source.server, "*search*", "search");
        Message row;
        row.id = "result-row";
        row.nick = "Peer";
        row.text = "[#source] Peer: remote hit";
        row.sourceBuffer = source.id;
        row.sourceServer = source.server;
        row.sourceTarget = source.target;
        row.sourceId = "remote-id";
        row.sourceText = "remote hit";
        row.time = "2026-11-01T12:00:00Z";
        row.presence = row.history = true;
        results.messages.push_back(row);
        app.model.activeBuffer = results.id;
        app.searchRequests_[source.server] = {"old-search", source.id, source.server, source.target,
                                              app.model.networks[source.server].sessionGeneration};
        Event searchBegin;
        searchBegin.type = "history_begin";
        searchBegin.server = source.server;
        searchBegin.target = source.target;
        searchBegin.label = "old-search";
        searchBegin.tags["direction"] = "search";
        app.reduce(searchBegin);
        assert(results.loading);
        app.action({"search", source.id, "", {}});
        assert(!results.loading && results.messages.empty());
        searchBegin.type = "history_end";
        app.reduce(searchBegin);
        assert(!results.loading && results.messages.empty());
        results.messages.push_back(row);
        const auto start = [&](const string &label) {
            app.searchJump_ = Application::SearchJump{
                label,         results.id,
                source.id,     source.server,
                source.target, {},
                row,           app.model.networks[source.server].sessionGeneration,
                app.model.now, 0};
            app.activeHistory_[label] = source.id;
            Application::HistoryPage page;
            page.server = source.server;
            page.buffer = source.id;
            page.session = app.model.networks[source.server].sessionGeneration;
            page.request.target = source.target;
            page.request.direction = HistoryDirection::Around;
            page.request.requestId = label;
            app.historyPages_[label] = std::move(page);
            source.loading = true;
        };
        Event end;
        end.type = "history_end";
        end.server = source.server;
        end.target = source.target;
        end.tags["direction"] = "around";
        const auto initialRevision = app.model.pendingSourceJump.revision;
        start("empty");
        end.label = "unrelated";
        app.reduce(end);
        assert(app.searchJump_ && source.loading);
        end.label = "empty";
        app.reduce(end);
        assert(!app.searchJump_ && !source.loading &&
               app.model.pendingSourceJump.revision == initialRevision &&
               app.model.activeBuffer == results.id);
        start("failed");
        end.label = "failed";
        end.tags["error"] = "denied";
        app.reduce(end);
        assert(!app.searchJump_ && !source.loading &&
               app.model.pendingSourceJump.revision == initialRevision);
        end.tags.erase("error");
        start("successful");
        Event original;
        original.type = "message";
        original.server = source.server;
        original.target = source.target;
        original.label = "successful";
        original.id = row.sourceId;
        original.nick = row.nick;
        original.text = row.sourceText;
        original.time = row.time;
        original.history = true;
        app.reduce(original);
        assert(app.searchJump_ && app.searchJump_->matches == 0 &&
               app.model.pendingSourceJump.revision == initialRevision);
        end.label = "successful";
        end.tags["oldest-id"] = end.tags["newest-id"] = original.id;
        end.tags["oldest-time"] = end.tags["newest-time"] = original.time;
        app.reduce(end);
        assert(!app.searchJump_ && !source.loading &&
               app.model.pendingSourceJump.revision == initialRevision + 1 &&
               app.model.pendingSourceJump.message == original.id &&
               app.model.activeBuffer == source.id && source.draft == "pending draft");
        app.model.activeBuffer = results.id;
        start("hidden");
        original.label = "hidden";
        app.reduce(original);
        app.model.settings.fools.insert(row.nick);
        app.model.settings.foolDisplay = FoolDisplay::Hidden;
        end.label = "hidden";
        app.reduce(end);
        assert(!app.searchJump_ && !source.loading &&
               app.model.pendingSourceJump.revision == initialRevision + 1 &&
               app.model.activeBuffer == results.id);
        app.model.settings.fools.clear();
        start("closed");
        const string sourceId = source.id;
        app.action({"close_buffer", sourceId, {}, {}});
        assert(!app.searchJump_ && !app.model.buffers.contains(sourceId));
        const auto closedRevision = app.model.pendingSourceJump.revision;
        original.label = "closed";
        app.reduce(original);
        end.label = "closed";
        app.reduce(end);
        assert(!app.model.buffers.contains(sourceId) &&
               app.model.pendingSourceJump.revision == closedRevision);
    }
    static void privateSojuSearch() {
        const auto setting = [](const char *key) {
            const char *value = std::getenv(key);
            if (!value)
                throw std::runtime_error(string("Missing private fixture setting: ") + key);
            return string(value);
        };
        const auto require = [](bool ok, const string &why) {
            if (!ok)
                throw std::runtime_error(why);
        };
        ServerConfig server;
        server.name = "private-soju-account";
        server.host = setting("BLAST_TEST_HOST");
        require(server.host == "localhost" || server.host == "127.0.0.1",
                "Search probe accepts only private loopback fixtures");
        server.port = std::stoi(setting("BLAST_TEST_PORT"));
        server.tls = true;
        server.bouncer = "soju";
        server.sojuAccount = true;
        Identity identity;
        identity.username = setting("BLAST_TEST_USERNAME");
        identity.username = identity.username.substr(0, identity.username.find('/'));
        identity.nick = setting("BLAST_TEST_NICK");
        identity.password = setting("BLAST_TEST_PASSWORD");
        server.sasl = "PLAIN";
        Application app;
        app.model = Model{};
        app.savingAllowed_ = true;
        app.model.identities = {identity};
        app.model.servers = {server};
        app.connect(server.name);
        string childName;
        set<std::pair<string, string>> initialHistoryEnded;
        size_t searchEnds = 0;
        const auto drain = [&] {
            vector<Event> incoming;
            for (auto &[name, connected] : app.clients_)
                for (auto &event : connected->poll())
                    incoming.push_back(std::move(event));
            for (auto &event : incoming) {
                if (event.type == "history_end" && event.tags["initial"] == "1" &&
                    event.tags["error"].empty())
                    initialHistoryEnded.emplace(event.server, event.target);
                if (event.server == childName && event.type == "history_end" &&
                    event.tags["direction"] == "search")
                    ++searchEnds;
                if (event.type == "history_begin" || event.type == "history_end" ||
                    event.tags.contains("search") || event.type == "error")
                    std::cout << "EVENT " << event.type << " target=" << event.target
                              << " text=" << event.text << " label=" << event.label
                              << " direction=" << event.tags["direction"] << std::endl;
                app.reduce(std::move(event));
            }
            app.scheduleHistory();
        };
        const auto wait = [&](const auto &predicate, const string &why) {
            const auto deadline = RenameDrainPeer::Clock::now() + std::chrono::seconds(20);
            do {
                drain();
                if (predicate())
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            } while (RenameDrainPeer::Clock::now() < deadline);
            throw std::runtime_error(why + ": " + app.model.status);
        };
        wait(
            [&] {
                for (const auto &profile : app.model.servers)
                    if (profile.sojuParent == server.name) {
                        childName = profile.name;
                        return app.model.networks[childName].state == "Ready";
                    }
                return false;
            },
            "Native Soju account catalog did not produce a ready BOUNCER-bound child");
        require(app.model.networks[server.name].sojuDiscoveryComplete,
                "Native Soju account catalog did not complete");
        require(app.model.networks[childName].caps.contains("soju.im/search"),
                "Native Soju child did not enable soju.im/search");
        auto *client = app.client(childName);
        std::cout << "CAP soju.im/search enabled on native child=" << childName
                  << " parent=" << server.name << " account=" << identity.username << '\n';
        const string channel = setting("BLAST_TEST_CHANNEL");
        const string marker = setting("BLAST_TEST_TOKEN") + " two words; punctuation!";
        const string secondMarker = setting("BLAST_TEST_TOKEN") + " second search; punctuation!";
        auto &source = app.buffer(childName, channel);
        client->command("JOIN " + channel);
        wait([&] { return source.joined; }, "Private child did not join search source");
        wait(
            [&] {
                return initialHistoryEnded.contains({childName, source.target}) &&
                       source.initialHistorySync == HistorySyncState::Idle && !source.loading;
            },
            "JOIN-triggered initial history did not complete before remote-only marker");
        const string beforeMarker = setting("BLAST_TEST_TOKEN") + " before context";
        client->send(channel, beforeMarker);
        wait(
            [&] {
                return std::any_of(
                    source.messages.begin(), source.messages.end(),
                    [&](const auto &m) { return m.text == beforeMarker && !m.pending; });
            },
            "Soju did not acknowledge preceding context");
        client->send(channel, marker);
        wait(
            [&] {
                return std::any_of(source.messages.begin(), source.messages.end(),
                                   [&](const auto &m) { return m.text == marker && !m.pending; });
            },
            "Soju did not acknowledge marker for remote storage");
        client->send(channel, secondMarker);
        wait(
            [&] {
                return std::any_of(
                    source.messages.begin(), source.messages.end(),
                    [&](const auto &m) { return m.text == secondMarker && !m.pending; });
            },
            "Soju did not acknowledge second remote marker");
        app.command(source, "/clear");
        require(source.messages.empty() && app.storage_.history(source.id, 100).empty(),
                "Local source must be empty before remote-only search");
        std::cout << "LOCAL source rows=0; query=" << marker << std::endl;
        app.command(source, "/search " + channel + " " + marker);
        auto &results = app.buffer(childName, "*search*", "search");
        require(results.messages.empty(), "Local fallback masked the remote-only probe");
        wait(
            [&] {
                return std::any_of(
                    results.messages.begin(), results.messages.end(),
                    [&](const auto &m) { return m.text.find(marker) != string::npos; });
            },
            "Remote-only marker did not reach Core-visible *search* rows");
        require(source.messages.empty(), "SEARCH polluted source history");
        for (const auto &row : results.messages)
            std::cout << "CORE *search* id=" << row.id << " text=" << row.text << '\n';
        wait([&] { return searchEnds == 1 && !results.loading; }, "First search did not complete");
        const string repeatOrigin = app.model.activeBuffer;
        require(repeatOrigin == results.id, "First search did not select result buffer");
        std::cout << "REPEAT search from active result buffer=" << repeatOrigin << std::endl;
        app.action({"search", repeatOrigin, secondMarker, {}});
        wait(
            [&] {
                return std::any_of(
                    results.messages.begin(), results.messages.end(),
                    [&](const auto &m) { return m.text.find(secondMarker) != string::npos; });
            },
            "Repeat search from active *search* did not reach original conversation");
        wait([&] { return searchEnds == 2 && !results.loading; }, "Repeat search did not complete");
        require(std::none_of(results.messages.begin(), results.messages.end(),
                             [&](const auto &m) { return m.text.find(marker) != string::npos; }),
                "Repeat search retained rows from the previous query");
        app.action({"search", results.id, setting("BLAST_TEST_TOKEN") + " absent-marker", {}});
        wait([&] { return searchEnds == 3 && !results.loading; },
             "Empty repeat search did not complete");
        require(results.messages.empty() && source.messages.empty(),
                "Empty repeat search retained results or polluted source history");
        // Search only supplies a presentation row. The jump must fetch actual
        // surrounding history and retain the source composer draft.
        source.draft = "unsent source draft";
        app.action({"search", results.id, marker, {}});
        wait([&] { return searchEnds == 4 && !results.loading; },
             "Context search did not complete");
        const auto selected = std::find_if(results.messages.begin(), results.messages.end(),
                                           [&](const auto &m) { return m.sourceText == marker; });
        require(selected != results.messages.end() && source.messages.empty(),
                "Context search did not produce a remote-only result");
        const auto revision = app.model.pendingSourceJump.revision;
        app.action({"search-jump", results.id, selected->id, {}});
        wait([&] { return app.model.pendingSourceJump.revision > revision; },
             "Remote-only result did not navigate after AROUND");
        require(app.model.activeBuffer == source.id && source.draft == "unsent source draft" &&
                    app.model.pendingSourceJump.buffer == source.id,
                "Remote jump lost source selection or draft");
        const auto match = std::find_if(source.messages.begin(), source.messages.end(),
                                        [&](const auto &m) { return m.text == marker; });
        require(match != source.messages.end() &&
                    app.model.pendingSourceJump.message == match->id &&
                    std::any_of(source.messages.begin(), source.messages.end(),
                                [&](const auto &m) { return m.text == beforeMarker; }) &&
                    std::any_of(source.messages.begin(), source.messages.end(),
                                [&](const auto &m) { return m.text == secondMarker; }),
                "AROUND did not restore the real selected message and its neighbors");
        client->stop();
        std::cout << "PASS private Soju remote-only multiword punctuation search" << std::endl;
    }
    static void commandArgumentContext() {
        const auto missing = [](std::string_view input, std::string_view kind,
                                std::string_view types = "#&+!") {
            return !missingCommandArguments(input, kind, types).empty();
        };
        for (const auto input :
             {"/me", "/msg", "/query", "/join", "/nick", "/raw", "/quote", "/bouncer",
              "/bouncer bind", "/mode", "/topic", "/search", "/search #room"})
            assert(missing(input, "server"));
        for (const auto input : {"/away",
                                 "/away lunch",
                                 "/connect",
                                 "/disconnect",
                                 "/clear",
                                 "/quit",
                                 "/names",
                                 "/list",
                                 "/who",
                                 "/msg Nick",
                                 "/query Nick",
                                 "/join #room",
                                 "/nick NewNick",
                                 "/raw VERSION",
                                 "/quote VERSION",
                                 "/bouncer bind 42",
                                 "/mode #room",
                                 "/topic #room",
                                 "/search #room words",
                                 "/ME waves"})
            assert(!missing(input, "server"));
        assert(missing("/whois", "channel") && missing("/whois", "server"));
        assert(!missing("/whois", "query") && !missing("/whois Nick", "server"));
        assert(!missing("/part", "channel") && !missing("/part reason", "channel"));
        assert(missing("/part", "query") && missing("/part reason", "server"));
        assert(!missing("/part #room", "server"));
        assert(missing("/kick", "channel") && missing("/kick Nick", "query"));
        assert(missing("/kick #room", "channel") && missing("/kick #room", "server"));
        assert(!missing("/kick Nick", "channel") && !missing("/kick #room Nick", "server"));
        assert(!missing("/part !room", "server", "!"));
        assert(!missing("/kick !room Nick", "server", "!"));
        assert(missing("/part #room", "server", "!"));
        assert(missing("/kick !room", "server", "!"));
        assert(!missing("/unknown", "server"));
        for (const auto &entry : commandCatalog) {
            assert(!entry.name.empty() && !entry.description.empty());
            assert(
                std::count_if(commandCatalog.begin(), commandCatalog.end(), [&](const auto &other) {
                    return commandEqual(entry.name, other.name);
                }) == 1);
        }
    }
    static void appOriginAfterSwitch() {
        RenameDrainPeer peer;
        Application app; // Destroy the worker before its loopback peer.
        app.model = Model{};
        app.savingAllowed_ = false;
        app.messageCount_ = 0;
        app.model.now = 100;
        app.model.settings.retentionDays = 0;
        Identity identity;
        identity.nick = "Me";
        ServerConfig server;
        server.name = "command-origin";
        server.host = "127.0.0.1";
        server.port = peer.port;
        server.tls = false;
        app.model.identities = {identity};
        app.model.servers = {server};
        app.clients_[server.name] = std::make_unique<NetworkClient>(server, identity);
        auto &client = *app.clients_.at(server.name);
        client.start();
        peer.accept();
        peer.expect("CAP", {"LS", "302"});
        peer.send(":server CAP * LS :message-tags labeled-response batch");
        peer.expect("NICK", {identity.nick});
        peer.expect("USER", {identity.username, "0", "*", identity.realname});
        const auto request = peer.read();
        assert(request.command == "CAP" && request.params.size() == 2 &&
               request.params[0] == "REQ" &&
               request.params[1].find("message-tags") != string::npos &&
               request.params[1].find("labeled-response") != string::npos);
        peer.send(":server CAP Me ACK :message-tags labeled-response batch");
        peer.expect("CAP", {"END"});
        peer.send(":server 001 Me :Welcome");
        peer.send(":server 376 Me :End of MOTD");
        const auto deadline = RenameDrainPeer::Clock::now() + std::chrono::seconds(3);
        while (app.model.networks[server.name].state != "Ready") {
            for (const auto &event : client.poll()) {
                if (event.type == "state" &&
                    (event.text == "Failed" || event.text == "Disconnected"))
                    throw std::runtime_error("Command-origin registration " + event.text);
                app.reduce(event);
            }
            RenameDrainPeer::require(RenameDrainPeer::Clock::now() < deadline,
                                     "Command-origin registration deadline expired");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        app.clear(nullptr);
        const string origin = app.buffer(server.name, "#origin").id;
        const string elsewhere = app.buffer(server.name, "#elsewhere").id;
        const string target = app.buffer(server.name, "#target").id;
        const string utility = app.buffer(server.name, "*server*", "server").id;
        const auto contains = [&](const string &id, std::string_view text) {
            const auto *b = app.findBuffer(id);
            return b && std::any_of(b->messages.begin(), b->messages.end(), [&](const auto &m) {
                       return m.text.find(text) != string::npos;
                   });
        };
        const auto drain = [&] {
            peer.fence();
            for (const auto &event : client.poll())
                app.reduce(event);
        };
        // Exercise composer metadata through the real application/network boundary.
        app.model.settings.typingSend = false;
        app.savingAllowed_ = true;
        Action localReply{"send", origin, "raw reply body", {}};
        localReply.attributes["reply-nick"] = "ParentNick";
        app.action(localReply);
        const auto replyFrame = peer.read();
        assert(replyFrame.command == "PRIVMSG" &&
               replyFrame.params == vector<string>({"#origin", "ParentNick: raw reply body"}) &&
               !replyFrame.tags.contains("+reply"));
        drain();
        assert(app.model.buffers.at(origin).messages.back().text == "ParentNick: raw reply body");
        app.savingAllowed_ = true;
        auto &originChat = app.model.buffers.at(origin);
        originChat.joined = true;
        app.model.activeBuffer = origin;
        app.action({"archive_buffer", origin, "on", {}});
        assert(originChat.archived && originChat.joined && app.model.activeBuffer == utility);
        drain(); // A PART (or any other archive wire command) would fail the PONG fence.
        app.action({"archive_buffer", origin, "off", {}});
        assert(!originChat.archived && originChat.joined);
        assert(app.save());
        Model restart;
        app.storage_.load(restart);
        assert(restart.buffers.at(origin).draft.empty() &&
               std::any_of(restart.buffers.at(origin).messages.begin(),
                           restart.buffers.at(origin).messages.end(), [](const auto &m) {
                               return m.text == "ParentNick: raw reply body" && !m.pending &&
                                      m.sendStatus == SendStatus::Sent;
                           }));
        Event rejected;
        rejected.type = "error";
        rejected.server = server.name;
        rejected.target = "#origin";
        rejected.label = originChat.messages.back().label;
        rejected.text = "synthetic fallback rejection";
        rejected.tags["send-failed"] = "1";
        rejected.tags["original-text"] = "raw reply body";
        app.reduce(rejected);
        assert(originChat.draft == "raw reply body");
        originChat.draft.clear();
        originChat.joined = false;
        app.savingAllowed_ = false;
        // Capture through the public command path, not by seeding the private route map.
        const auto submit = [&](const string &id, const string &input) {
            const auto previous = app.commandOrigins_;
            app.model.activeBuffer = id;
            app.command(app.model.buffers.at(id), input);
            const auto route =
                std::find_if(app.commandOrigins_.begin(), app.commandOrigins_.end(),
                             [&](const auto &item) { return !previous.contains(item.first); });
            assert(route != app.commandOrigins_.end() && route->second.buffer == id);
            const string commandId = route->first;
            const auto wire = peer.read();
            assert(wire.tags.contains("label") && !wire.tags.at("label").empty());
            // Navigation is an independent user action, before the server answers.
            app.model.activeBuffer = elsewhere;
            return std::pair{wire.tags.at("label"), commandId};
        };
        const auto reply = [&](const string &label, const string &line) {
            peer.send("@label=" + label + " " + line);
            drain();
        };

        auto command = submit(origin, "/whois Alice");
        peer.send(":irc.example NOTICE Me :unsolicited-notice");
        peer.send(":irc.example 372 Me :unsolicited-server-line");
        drain();
        assert(contains(utility, "unsolicited-notice") &&
               contains(utility, "unsolicited-server-line"));
        assert(!contains(origin, "unsolicited-") && !contains(elsewhere, "unsolicited-"));
        reply(command.first, ":server 311 Me Alice user host * :origin-whois");
        const auto &profile = app.model.whoisProfile;
        assert(profile.open && !profile.loading && profile.error.empty() &&
               profile.requestId == command.second && profile.server == server.name &&
               profile.target == "Alice" && profile.records.size() == 1);
        assert(profile.records.front().nick == "Alice" &&
               profile.records.front().username == "user" &&
               profile.records.front().host == "host" &&
               profile.records.front().realName == "origin-whois");
        assert(!contains(origin, "origin-whois") && !contains(elsewhere, "origin-whois") &&
               !contains(utility, "origin-whois"));
        assert(app.model.activeBuffer == elsewhere &&
               !app.commandOrigins_.contains(command.second));

        // A real labeled batch preserves parameter boundaries and each result's identity.
        command = submit(origin, "/whois irc.example [Alice],Bob");
        assert(profile.open && profile.loading && profile.records.empty() &&
               profile.requestId == command.second && profile.target == "[Alice],Bob" &&
               profile.sessionGeneration == app.model.networks[server.name].sessionGeneration);
        const auto loadingRevision = profile.revision;
        peer.send("@label=" + command.first + " :server BATCH +profile labeled-response");
        peer.send("@batch=profile :server 311 Me [Alice] alice host-a * :Alice Real Name");
        peer.send("@batch=profile :server 312 Me {aLICE} irc.example :Server with spaces");
        peer.send("@batch=profile :server 330 Me {alice} account-a :is logged in as");
        peer.send("@batch=profile :server 301 Me [ALICE] :Away with spaces");
        peer.send("@batch=profile :server 317 Me [Alice] 42 123456 :seconds idle, signon time");
        peer.send("@batch=profile :server 319 Me [Alice] :@#one +#two");
        peer.send("@batch=profile :server 319 Me {alice} :#three");
        peer.send("@batch=profile :server 313 Me [Alice] :is an IRC operator");
        peer.send("@batch=profile :server 671 Me [Alice] :is using a secure connection");
        peer.send("@batch=profile :server 311 Me Bob bob host-b * :Bob Real Name");
        peer.send("@batch=profile :server 330 Me Bob account-b :is logged in as");
        drain();
        assert(profile.loading && profile.revision > loadingRevision &&
               profile.records.size() == 2);
        const auto &first = profile.records[0];
        const auto &second = profile.records[1];
        assert(first.nick == "[Alice]" && first.username == "alice" && first.host == "host-a" &&
               first.realName == "Alice Real Name" && first.account == "account-a" &&
               first.server == "irc.example" && first.serverInfo == "Server with spaces" &&
               first.away == "Away with spaces" && first.idle == "42" && first.signon == "123456" &&
               first.channels.find("@#one +#two") != string::npos &&
               first.channels.find("#three") != string::npos);
        assert(first.details.size() == 2 &&
               std::any_of(first.details.begin(), first.details.end(),
                           [](const auto &detail) {
                               return detail.find("IRC operator") != string::npos;
                           }) &&
               std::any_of(first.details.begin(), first.details.end(), [](const auto &detail) {
                   return detail.find("secure connection") != string::npos;
               }));
        assert(second.nick == "Bob" && second.username == "bob" && second.host == "host-b" &&
               second.realName == "Bob Real Name" && second.account == "account-b" &&
               second.away.empty() && second.server.empty() && second.channels.empty() &&
               second.details.empty());
        const auto fieldsRevision = profile.revision;
        peer.send(":server BATCH -profile"); // No 318: batch completion is authoritative.
        drain();
        assert(profile.open && !profile.loading && profile.error.empty() &&
               profile.revision > fieldsRevision && !app.commandOrigins_.contains(command.second));
        for (const auto &id : {origin, elsewhere, utility}) {
            assert(!contains(id, "Alice Real Name") && !contains(id, "Bob Real Name") &&
                   !contains(id, "Away with spaces") && !contains(id, "Server with spaces"));
        }

        // Reducer boundary: diagnostics are lossy and must never replace Event.params.
        const auto response = [&](const std::pair<string, string> &request, const string &numeric,
                                  vector<string> params, bool complete = false) {
            Event event;
            event.type = "network";
            event.server = server.name;
            event.params = std::move(params);
            event.tags = {{"command-id", request.second},
                          {"command", numeric},
                          {"command-response", numeric + " forged flattened identity"}};
            if (complete)
                event.tags["command-complete"] = "1";
            return event;
        };
        command = submit(origin, "/whois Params");
        app.reduce(response(command, "311", {"Me", "Params"}));
        assert(profile.records.empty());
        app.reduce(response(command, "311",
                            {"Me", "SomeoneElse", "wrong", "host", "*", "wrong target identity"}));
        assert(profile.records.empty());
        auto parsed =
            response(command, "311",
                     {"Me", "Params", "exact-user", "exact-host", "*", "Real name : with  spaces"});
        auto unrelated = parsed;
        unrelated.tags["command-id"] = "unknown-whois-request";
        app.reduce(unrelated);
        assert(profile.records.empty() && profile.loading);
        app.reduce(parsed);
        assert(profile.records.size() == 1 && profile.records[0].username == "exact-user" &&
               profile.records[0].realName == "Real name : with  spaces");
        const auto parsedRevision = profile.revision;
        auto foreign = response(command, "330", {"Me", "Params", "wrong-account", "logged in"});
        foreign.server = "other-server";
        app.reduce(foreign);
        assert(profile.revision == parsedRevision && profile.records[0].account.empty() &&
               app.commandOrigins_.contains(command.second));
        app.reduce(response(command, "311", {"Me", "Params"}));
        app.reduce(response(command, "317", {"Me", "Params"}));
        assert(profile.records[0].username == "exact-user" && profile.records[0].idle.empty());
        app.reduce(response(command, "317", {"Me", "Params", "7"}));
        assert(profile.records[0].idle == "7" && profile.records[0].signon.empty());
        reply(command.first, ":server ACK");
        assert(!profile.loading && profile.error.empty() && profile.records.size() == 1);
        assert(!contains(origin, "forged flattened identity"));

        // Superseded and explicitly closed profiles cannot be reopened by late output.
        const auto superseded = submit(origin, "/whois Old");
        command = submit(origin, "/whois Current");
        const auto currentRevision = profile.revision;
        reply(superseded.first, ":server 311 Me Old old old-host * :obsolete-identity");
        assert(profile.requestId == command.second && profile.records.empty() &&
               profile.revision == currentRevision && profile.loading);
        app.model.whoisProfile.open = false;
        reply(command.first, ":server 311 Me Current user host * :closed-identity");
        assert(!profile.open && profile.records.empty() && !profile.loading);
        assert(!contains(origin, "obsolete-identity") && !contains(origin, "closed-identity"));

        for (const auto numeric : {"401", "402", "421", "461", "263"}) {
            command = submit(origin, "/whois Missing");
            const string targetParam =
                string(numeric) == "401" || string(numeric) == "402" ? "Missing" : "WHOIS";
            reply(command.first,
                  ":server " + string(numeric) + " Me " + targetParam + " :profile failure detail");
            assert(profile.open && !profile.loading && !profile.error.empty() &&
                   profile.records.empty() && !app.commandOrigins_.contains(command.second));
            assert(!contains(origin, "profile failure detail") &&
                   !contains(utility, "profile failure detail"));
        }
        command = submit(origin, "/whois Partial");
        peer.send("@label=" + command.first + " :server BATCH +profile-fail labeled-response");
        peer.send("@batch=profile-fail :server 311 Me Partial user host * :Partial real name");
        drain();
        assert(profile.loading && profile.records.size() == 1 &&
               profile.records[0].realName == "Partial real name");
        peer.send("@batch=profile-fail :server FAIL WHOIS LOOKUP_FAILED Partial "
                  ":profile lookup denied after partial result");
        peer.send(":server BATCH -profile-fail");
        drain();
        assert(profile.open && !profile.loading &&
               profile.error.find("profile lookup denied after partial result") != string::npos &&
               profile.records.size() == 1 && profile.records[0].realName == "Partial real name" &&
               !app.commandOrigins_.contains(command.second));
        for (const auto &id : {origin, elsewhere, utility})
            assert(!contains(id, "profile lookup denied after partial result") &&
                   !contains(id, "Partial real name"));

        command = submit(origin, "/whois Empty");
        reply(command.first, ":server ACK");
        assert(profile.open && !profile.loading && profile.records.empty());

        // Synthetic timeout has neither a numeric nor 318, but is still terminal.
        command = submit(origin, "/whois TimedOut");
        Event timeout;
        timeout.type = "network";
        timeout.server = server.name;
        timeout.tags = {{"kind", "command-complete"},
                        {"command-id", command.second},
                        {"command-complete", "1"},
                        {"error", "Command response timed out"}};
        app.reduce(timeout);
        assert(profile.open && !profile.loading && !profile.error.empty() &&
               !app.commandOrigins_.contains(command.second));
        const auto timeoutRevision = profile.revision;
        reply(command.first, ":server 311 Me TimedOut late host * :late-timeout-identity");
        assert(profile.revision == timeoutRevision && profile.records.empty() &&
               !contains(origin, "late-timeout-identity"));

        command = submit(origin, "/topic #target");
        reply(command.first, ":server 332 Me #target :actual-target-topic");
        assert(app.model.buffers.at(target).topic == "actual-target-topic");
        assert(app.model.buffers.at(origin).topic.empty());
        assert(contains(origin, "332 #target actual-target-topic"));
        assert(!contains(elsewhere, "actual-target-topic"));
        assert(!contains(target, "actual-target-topic"));
        command = submit(origin, "/topic #target");
        reply(command.first, ":server 331 Me #target :No topic is set");
        assert(app.model.buffers.at(target).topic.empty());
        assert(contains(origin, "331 #target No topic is set"));
        assert(!contains(target, "No topic is set"));

        // Topic protocol events update only header state; existing transcript rows stay intact.
        Message storedTopic;
        storedTopic.id = "stored-topic-announcement";
        storedTopic.nick = "Alice";
        storedTopic.text = "Alice changed the topic: stored older announcement";
        storedTopic.presence = true;
        storedTopic.history = true;
        app.insert(app.model.buffers.at(target), storedTopic, false);
        const auto targetMessages = app.model.buffers.at(target).messages.size();
        peer.send(":server 332 Me #target :initial snapshot");
        drain();
        assert(app.model.buffers.at(target).topic == "initial snapshot");
        assert(app.model.buffers.at(target).messages.size() == targetMessages);
        peer.send(":Alice!user@host TOPIC #target :live topic change");
        drain();
        assert(app.model.buffers.at(target).topic == "live topic change");
        assert(app.model.buffers.at(target).messages.size() == targetMessages);
        assert(!contains(target, "live topic change"));
        peer.send(":server 332 Me #target :refreshed snapshot");
        peer.send(":server 331 Me #target :No topic is set");
        drain();
        assert(app.model.buffers.at(target).topic.empty());
        assert(app.model.buffers.at(target).messages.size() == targetMessages);
        peer.send(":Alice!user@host TOPIC #target :");
        drain();
        assert(app.model.buffers.at(target).topic.empty());
        assert(app.model.buffers.at(target).messages.size() == targetMessages);

        const auto unsolicitedState = app.model.buffers.at(target).historyState;
        app.savingAllowed_ = true; // Completed unsolicited pages use the real writer too.
        peer.send(":server 332 Me #target :current live topic");
        peer.send(":server BATCH +topic-history chathistory #target");
        peer.send("@batch=topic-history :Alice!user@host TOPIC #target :historic topic change");
        peer.send("@batch=topic-history :server 332 Me #target :historic snapshot");
        peer.send("@batch=topic-history :Bob!user@host PRIVMSG #target "
                  ":Alice changed the topic: arbitrary historical chat");
        peer.send("@batch=topic-history :server 331 Me #target :No historic topic");
        drain();
        assert(app.model.buffers.at(target).messages.size() == targetMessages);
        assert(!contains(target, "arbitrary historical chat"));
        peer.send(":server BATCH -topic-history");
        drain();
        const auto &afterUnsolicited = app.model.buffers.at(target).historyState;
        assert(afterUnsolicited.oldest == unsolicitedState.oldest &&
               afterUnsolicited.newest == unsolicitedState.newest &&
               afterUnsolicited.beginning == unsolicitedState.beginning &&
               afterUnsolicited.gaps.size() == unsolicitedState.gaps.size());
        for (size_t i = 0; i < unsolicitedState.gaps.size(); ++i)
            assert(afterUnsolicited.gaps[i].id == unsolicitedState.gaps[i].id &&
                   afterUnsolicited.gaps[i].older == unsolicitedState.gaps[i].older &&
                   afterUnsolicited.gaps[i].newer == unsolicitedState.gaps[i].newer &&
                   afterUnsolicited.gaps[i].unavailable == unsolicitedState.gaps[i].unavailable);
        assert(app.model.buffers.at(target).topic == "current live topic");
        assert(!contains(target, "historic topic change"));
        assert(app.model.buffers.at(target).messages.size() == targetMessages + 1);
        assert(!contains(target, "historic snapshot") && !contains(target, "No historic topic"));
        peer.send(":Bob!user@host PRIVMSG #target :Alice changed the topic: arbitrary chat");
        drain();
        assert(app.model.buffers.at(target).topic == "current live topic");
        const auto &topicMessages = app.model.buffers.at(target).messages;
        assert(std::any_of(topicMessages.begin(), topicMessages.end(), [](const auto &message) {
            return message.nick == "Bob" && !message.presence &&
                   message.text == "Alice changed the topic: arbitrary chat";
        }));
        assert(std::any_of(topicMessages.begin(), topicMessages.end(), [](const auto &message) {
            return message.nick == "Bob" && message.history && !message.presence &&
                   message.text == "Alice changed the topic: arbitrary historical chat";
        }));
        assert(std::any_of(topicMessages.begin(), topicMessages.end(), [&](const auto &message) {
            return message.id == storedTopic.id && message.text == storedTopic.text &&
                   message.presence && message.history;
        }));
        Event unowned;
        unowned.type = "message";
        unowned.server = server.name;
        unowned.target = "#target";
        unowned.label = "arbitrary-unowned-history";
        unowned.id = "must-not-adopt-explicit-label";
        unowned.text = "unknown labeled history must not materialize";
        unowned.history = true;
        app.reduce(unowned);
        assert(!contains(target, unowned.text) &&
               !app.storage_.message(target, unowned.id).has_value());
        app.savingAllowed_ = false;

        command = submit(origin, "/names #target");
        reply(command.first, ":server 353 Me = #target :Alice Bob");
        assert(app.model.buffers.at(target).nicks.size() == 2);
        assert(app.model.buffers.at(origin).nicks.empty());
        assert(contains(origin, "353 = #target Alice Bob"));
        // Complete the actual NAMES snapshot even though the standalone labeled reply completed.
        peer.send(":server 366 Me #target :End of NAMES");
        drain();
        assert(!app.namesInProgress_.contains(target));

        command = submit(origin, "/whois Alice");
        reply(command.first, ":server 301 Me Alice :away-at-lunch");
        const auto &members = app.model.buffers.at(target).nicks;
        const auto alice = std::find_if(members.begin(), members.end(),
                                        [](const auto &nick) { return nick.name == "Alice"; });
        assert(alice != members.end() && alice->away);
        assert(profile.records.size() == 1 && profile.records[0].away == "away-at-lunch" &&
               !contains(origin, "301 Alice away-at-lunch"));

        command = submit(origin, "/mode #target +o Alice");
        reply(command.first, ":Operator!user@host MODE #target +o Alice");
        assert(alice->prefix == "@");
        assert(contains(origin, "MODE #target +o Alice"));
        command = submit(origin, "/mode #target");
        reply(command.first, ":server 324 Me #target +nt");
        assert(alice->prefix == "@" && app.model.buffers.at(origin).nicks.empty());
        assert(contains(origin, "324 #target +nt") && !contains(utility, "324 #target"));
        assert(app.model.activeBuffer == elsewhere);
        const auto beforeAck = app.model.buffers.at(origin).messages.size();
        command = submit(origin, "/raw VERSION");
        reply(command.first, ":server ACK");
        assert(!app.commandOrigins_.contains(command.second) &&
               app.model.buffers.at(origin).messages.size() == beforeAck);

        command = submit(origin, "/whois FileHost");
        peer.send(":server 005 Me soju.im/FILEHOST=https://files.example :are supported");
        drain();
        assert(app.model.networks[server.name].fileHost == "https://files.example" &&
               app.commandOrigins_.contains(command.second));
        reply(command.first, ":server 311 Me FileHost user host * :after-filehost-update");
        assert(profile.records.size() == 1 &&
               profile.records[0].realName == "after-filehost-update" &&
               !contains(origin, "after-filehost-update") &&
               !contains(elsewhere, "after-filehost-update"));

        // Remap both a move and a merge while commands are in flight.
        command = submit(origin, "/raw VERSION");
        const string renamed = server.name + "/#renamed";
        app.renameBuffer(origin, "#renamed");
        assert(!app.findBuffer(origin) && app.commandOrigins_.at(command.second).buffer == renamed);
        reply(command.first, ":server 351 Me test.server :after-rename");
        assert(contains(renamed, "after-rename") && !app.findBuffer(origin));
        const string merged = app.buffer(server.name, "#merged").id;
        command = submit(renamed, "/raw VERSION");
        app.renameBuffer(renamed, "#merged");
        assert(!app.findBuffer(renamed) && app.commandOrigins_.at(command.second).buffer == merged);
        reply(command.first, ":server 351 Me test.server :after-merge");
        assert(contains(merged, "after-merge") && contains(merged, "after-rename"));

        command = submit(merged, "/raw VERSION");
        app.action({"close_buffer", merged, {}, {}});
        assert(!app.findBuffer(merged) && !app.commandOrigins_.contains(command.second));
        reply(command.first, ":server 351 Me test.server :after-close");
        assert(!app.findBuffer(merged) && !contains(elsewhere, "after-close"));

        assert(app.buffer(server.name, "#origin").id == origin);
        // Reconnect/failed/disconnect must retire captures, including reuse of the same buffer.
        for (const auto state : {"Connecting", "Disconnected", "Failed"}) {
            // Keep the non-WHOIS origin lifetime regression independent of profiles.
            const auto ordinary = submit(origin, "/raw VERSION");
            command = submit(origin, "/whois Retired");
            const auto generation = app.model.networks[server.name].sessionGeneration;
            Event event;
            event.type = "state";
            event.server = server.name;
            event.text = state;
            app.reduce(event);
            assert(app.model.networks[server.name].sessionGeneration > generation &&
                   !app.commandOrigins_.contains(command.second) &&
                   !app.commandOrigins_.contains(ordinary.second) && !profile.loading);
            const auto retiredRevision = profile.revision;
            reply(command.first, ":server 311 Me Retired user host * :retired-response");
            reply(ordinary.first, ":server 351 Me test.server :retired-version");
            assert(profile.records.empty() && profile.revision == retiredRevision &&
                   !contains(origin, "retired-response") &&
                   !contains(elsewhere, "retired-response") &&
                   !contains(origin, "retired-version") && !contains(elsewhere, "retired-version"));
        }

        // Local validation never enqueues a command or leaves an origin waiting for a reply.
        const auto beforeReject = app.commandOrigins_.size();
        for (const auto input : {"/join", "/kick", "/nick", "/raw", "/msg", "/whois"}) {
            const auto originMessages = app.model.buffers.at(origin).messages.size();
            const auto utilityMessages = app.model.buffers.at(utility).messages.size();
            const auto elsewhereMessages = app.model.buffers.at(elsewhere).messages.size();
            app.model.status.clear();
            app.command(app.model.buffers.at(origin), input);
            assert(!app.model.status.empty() && app.commandOrigins_.size() == beforeReject);
            assert(app.model.buffers.at(origin).messages.size() == originMessages + 1 &&
                   app.model.buffers.at(utility).messages.size() == utilityMessages &&
                   app.model.buffers.at(elsewhere).messages.size() == elsewhereMessages);
        }
        const auto beforeMessages = app.model.buffers.at(origin).messages.size();
        app.command(app.model.buffers.at(origin), "/raw SEARCH #target words");
        assert(app.commandOrigins_.size() == beforeReject + 1);
        app.model.activeBuffer = elsewhere;
        const auto rejectionDeadline = RenameDrainPeer::Clock::now() + std::chrono::seconds(3);
        while (app.commandOrigins_.size() != beforeReject) {
            for (const auto &event : client.poll())
                app.reduce(event);
            RenameDrainPeer::require(RenameDrainPeer::Clock::now() < rejectionDeadline,
                                     "Command-origin local rejection deadline expired");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(app.model.buffers.at(origin).messages.size() == beforeMessages + 1);
        assert(app.model.activeBuffer == elsewhere);
        drain(); // No rejected SEARCH frame may have escaped to the peer.
        // A fully written no-echo reply must not resurrect on a real disconnect/reload.
        app.savingAllowed_ = true;
        app.model.networks[server.name].state = "Ready";
        localReply.text = "successful noecho reply";
        app.action(localReply);
        peer.expect("PRIVMSG", {"#origin", "ParentNick: successful noecho reply"});
        drain();
        assert(app.model.buffers.at(origin).draft.empty() && app.queuedSends_.empty());
        app.stopClient(server.name);
        assert(app.model.buffers.at(origin).draft.empty());
        assert(app.save());
        app.storage_.load(restart);
        assert(restart.buffers.at(origin).draft.empty() &&
               std::any_of(restart.buffers.at(origin).messages.begin(),
                           restart.buffers.at(origin).messages.end(), [](const auto &m) {
                               return m.text == "ParentNick: successful noecho reply" &&
                                      !m.pending && m.sendStatus == SendStatus::Sent;
                           }));
    }
    static void localOriginErrors() {
        Application app;
        app.model = Model{};
        app.savingAllowed_ = false;
        app.messageCount_ = 0;
        ServerConfig server;
        server.name = "local-origin-errors";
        app.model.servers = {server};
        auto &origin = app.buffer(server.name, "#origin");
        auto &utility = app.buffer(server.name, "*server*", "server");
        auto &elsewhere = app.buffer(server.name, "#elsewhere");
        app.model.activeBuffer = elsewhere.id;
        for (const auto input : {"/connect nonexistent", "/me disconnected action",
                                 "/msg OfflineNick disconnected message"}) {
            const auto before = origin.messages.size();
            const auto utilityBefore = utility.messages.size();
            app.command(origin, input);
            assert(origin.messages.size() == before + 1 && origin.messages.back().presence &&
                   origin.messages.back().text == app.model.status);
            assert(utility.messages.size() == utilityBefore && elsewhere.messages.empty());
            assert(app.model.activeBuffer != origin.id && app.commandOrigins_.empty());
            assert(!app.findBuffer("nonexistent/*server*"));
        }
        assert(origin.draft == "/me disconnected action");
        auto &results = app.buffer(server.name, "*search*", "search");
        const auto counted = app.messageCount_;
        const auto before = results.messages.size();
        app.command(results, "/join");
        assert(results.messages.size() == before + 1 &&
               results.messages.back().text == app.model.status);
        assert(app.messageCount_ == counted && utility.messages.empty() &&
               app.commandOrigins_.empty());
    }
    static void closedQueuedCommandOrigin() {
        Application app;
        app.model = Model{};
        app.savingAllowed_ = false;
        ServerConfig first;
        first.name = "queued-origin-first";
        ServerConfig second;
        second.name = "queued-origin-second";
        app.model.servers = {first, second};
        Identity identity;
        // A client makes accidental fallback observable: JOIN would mutate the first profile
        // and create both a utility buffer and a route, even without starting its transport.
        app.clients_[first.name] = std::make_unique<NetworkClient>(first, identity);
        const string elsewhere = app.buffer(first.name, "#elsewhere").id;
        const string closed = app.buffer(second.name, "#closed").id;
        app.model.activeBuffer = elsewhere;
        app.model.actions = {{"close_buffer", closed, {}, {}},
                             {"command", closed, "/join #wrong-server", {}}};
        app.dispatchActions();
        assert(app.model.actions.empty() && !app.findBuffer(closed));
        assert(!app.findBuffer(first.name + "/*server*") &&
               !app.findBuffer(second.name + "/*server*"));
        assert(!app.findBuffer(first.name + "/#wrong-server") &&
               !app.findBuffer(second.name + "/#wrong-server"));
        assert(app.model.servers[0].channels.empty() && app.model.servers[1].channels.empty());
        assert(app.commandOrigins_.empty() && app.model.activeBuffer == elsewhere);
        assert(app.model.buffers.size() == 1 && !app.model.status.empty());
    }
    static void networkStatusErrors() {
        Application app;
        app.model = Model{};
        app.savingAllowed_ = false;
        ServerConfig parent;
        parent.name = "status-parent";
        parent.sojuAccount = true;
        ServerConfig child;
        child.name = "status-child";
        child.sojuParent = parent.name;
        child.sojuNetworkId = "42";
        app.model.servers = {parent, child};

        Event failure;
        failure.type = "state";
        failure.server = parent.name;
        failure.text = "Failed";
        failure.tags["error"] = "Parent connection rejected";
        app.reduce(failure);
        auto &control = app.model.networks.at(parent.name);
        assert(control.state == "Failed" && control.error == failure.tags.at("error"));
        assert(!app.model.networks.contains(child.name));

        failure.server = child.name;
        failure.tags["error"] = "Child attachment rejected";
        app.reduce(failure);
        auto &live = app.model.networks.at(child.name);
        assert(live.state == "Failed" && live.error == failure.tags.at("error"));
        assert(live.error != control.error);
        assert(app.model.status == live.error);

        app.disconnect(child.name);
        assert(live.state == "Disconnected" && live.error.empty());
        assert(control.state == "Failed" && !control.error.empty());
        // The global report is historical, not the child's current connection error.
        assert(!app.model.status.empty());

        app.reduce(failure);
        app.disconnect(parent.name);
        assert(control.state == "Disconnected" && control.error.empty());
        assert(live.state == "Disconnected" && live.error.empty());
    }
    static void sojuReconnectCatalog() {
        Application app;
        app.model = Model{};
        app.savingAllowed_ = false;
        Identity identity;
        app.model.identities = {identity};
        ServerConfig parent;
        parent.name = "reconnect-parent";
        parent.host = "127.0.0.1";
        parent.port = 9;
        parent.tls = false;
        parent.bouncer = "soju";
        parent.sojuAccount = true;
        ServerConfig child = parent;
        child.name = "retained-offline-deletion";
        child.sojuAccount = false;
        child.sojuParent = parent.name;
        child.sojuNetworkId = "42";
        child.bouncer = "soju:42";
        app.model.servers = {parent, child};
        assert(serverDisplayName(app.model, parent.name) == parent.name);
        assert(serverDisplayName(app.model, "unconfigured-orphan") == "Network");
        app.model.servers[0].alias = "Personal account";
        assert(serverDisplayName(app.model, parent.name) == "Personal account");
        app.model.servers[0].alias.clear();
        auto &room = app.buffer(child.name, "#kept");
        room.draft = "still local";
        auto &control = app.model.networks[parent.name];
        control.sojuSessionKey = sojuSessionKey(parent, identity);
        control.sojuNetworks["42"] = {"42", {{"name", "Deleted while offline"}}};
        control.sojuDiscoveryComplete = true;
        Event state;
        state.type = "state";
        state.server = parent.name;
        state.text = "Connecting";
        app.reduce(state);
        assert(control.sojuNetworks.empty() && !control.sojuDiscoveryComplete);
        state.text = "Ready";
        app.reduce(state);
        assert(!app.client(child.name) && app.model.servers.size() == 2);
        app.connect(child.name); // Explicit attachment cannot bypass current discovery.
        assert(!app.client(child.name));
        control.sojuNetworks["42"] = {"42", {{"state", "connected"}}};
        control.sojuDiscoveryComplete = true;
        app.action({"disconnect", {}, child.name, {}});
        assert(!app.model.servers[1].sojuEnabled);
        assert(control.sojuNetworks.at("42").attributes.at("state") == "connected");
        app.synchronizeSoju(parent.name);
        assert(!app.client(child.name)); // Catalog updates cannot undo a local disconnect.
        assert(serverDisplayName(app.model, child.name) == "Network");
        app.model.settings.retentionDays = 0;
        app.model.networks[child.name].nick = "Me";
        Event incoming;
        incoming.type = "message";
        incoming.server = child.name;
        incoming.target = room.target;
        incoming.nick = "Speaker";
        incoming.text = "Me: retained catalog source";
        incoming.id = "catalog-display-source";
        incoming.timestamp = 1793836800;
        incoming.time = "2026-11-05T00:00:00Z";
        app.reduce(incoming);
        const string roomId = room.id;
        auto assertRouting = [&] {
            assert(app.model.servers.size() == 2 && app.model.servers[1].name == child.name &&
                   app.model.servers[1].sojuParent == parent.name &&
                   app.model.servers[1].sojuNetworkId == "42");
            assert(room.id == roomId && room.server == child.name &&
                   roomId == child.name + "/#kept" && &app.model.buffers.at(roomId) == &room &&
                   !app.client(child.name));
            app.refreshMentions();
            const auto &feed = app.model.buffers.at(GlobalMentionsBufferId);
            const auto source =
                std::find_if(feed.messages.begin(), feed.messages.end(),
                             [&](const auto &message) { return message.sourceId == incoming.id; });
            assert(source != feed.messages.end() && source->sourceBuffer == roomId &&
                   source->sourceServer == child.name && source->sourceTarget == room.target);
            assert(resolveMessageSource(app.model, feed, *source) == &room);
        };
        assertRouting();
        Event row;
        row.type = "network";
        row.server = parent.name;
        row.id = "42";
        row.tags = {{"kind", "bouncer"}, {"name", "Late catalog name"}};
        app.reduce(row);
        assert(serverDisplayName(app.model, child.name) == "Late catalog name");
        assert(app.model.servers[1].alias.empty());
        assertRouting();
        row.tags["name"] = "Renamed network";
        app.reduce(row);
        assert(serverDisplayName(app.model, child.name) == "Renamed network");
        assert(app.model.servers[1].alias.empty());
        assertRouting();
        app.command(room, "/join #other");
        assert(app.model.status == "Not connected to Renamed network" &&
               room.messages.back().text == app.model.status);
        row.tags["name"].clear();
        app.reduce(row);
        assert(serverDisplayName(app.model, child.name) == "Network");
        assert(app.model.servers[1].alias.empty());
        assertRouting();
        app.model.servers[1].alias = "My network";
        row.tags["name"] = "Another catalog name";
        app.reduce(row);
        assert(app.model.servers[1].alias == "My network" &&
               serverDisplayName(app.model, child.name) == "My network");
        assertRouting();
        app.send(room, "offline draft", {}, false, roomId);
        assert(app.model.status == "Not connected to My network" &&
               room.messages.back().text == app.model.status);
        app.connect("unconfigured-orphan", roomId);
        assert(app.model.status == "Unknown server: Network" &&
               room.messages.back().text == app.model.status &&
               !app.model.buffers.contains("unconfigured-orphan/*server*"));
        room.draft = "still local";
        control.sojuDiscoveryComplete = false;
        app.action({"connect", {}, child.name, {}});
        assert(app.model.servers[1].sojuEnabled && !app.client(child.name));
        app.command(room, "/disconnect");
        assert(!app.model.servers[1].sojuEnabled);
        app.command(room, "/connect");
        assert(app.model.servers[1].sojuEnabled && !app.client(child.name));
        assert(control.sojuNetworks.at("42").attributes.at("state") == "connected");
        control.sojuNetworks.clear();
        Event batch;
        batch.type = "network";
        batch.server = parent.name;
        batch.id = "current-empty";
        batch.tags = {{"kind", "bouncer-list"}, {"phase", "begin"}};
        app.reduce(batch);
        batch.tags["phase"] = "end";
        app.reduce(batch);
        assert(control.sojuDiscoveryComplete && app.model.servers.size() == 1);
        assert(!app.client(child.name) && room.draft == "still local");
    }
    static void sojuHierarchy() {
        Application app;
        app.model = Model{};
        app.savingAllowed_ = false;
        app.shuttingDown_ = true; // Reducer checks do not open sockets.
        Identity identity;
        identity.username = "account";
        identity.password = "private";
        app.model.identities = {identity};
        ServerConfig parent;
        parent.name = "account-key";
        parent.host = "bouncer.example";
        parent.bouncer = "soju";
        parent.sasl = "PLAIN";
        ServerConfig child = parent;
        child.name = "stable-old-key";
        child.bouncer = "soju:42";
        child.sojuEnabled = false;
        app.model.servers = {parent, child};
        auto &room = app.buffer(child.name, "#room");
        room.draft = "retain draft";
        room.readMarker = "2026-01-01T00:00:00Z";
        Message old;
        old.id = "retained";
        old.text = "local history";
        room.messages.push_back(old);
        app.adoptSojuProfiles();
        assert(app.model.servers[0].sojuAccount);
        assert(app.model.servers[1].name == "stable-old-key");
        assert(app.model.servers[1].sojuParent == parent.name);
        assert(room.id == "stable-old-key/#room");
        Event row;
        row.type = "network";
        row.server = parent.name;
        row.id = "42";
        row.tags = {{"kind", "bouncer"}, {"name", "Initial"}, {"host", "irc.example"}};
        app.reduce(row);
        row.tags = {{"kind", "bouncer"}, {"state", "connected"}, {"host", ""}};
        app.reduce(row);
        const auto &attrs = app.model.networks[parent.name].sojuNetworks.at("42").attributes;
        assert(attrs.at("name") == "Initial" && attrs.at("state") == "connected");
        assert(!attrs.contains("host"));
        row.server = child.name;
        row.id = "untrusted-child-catalog";
        app.reduce(row);
        assert(app.model.networks[child.name].sojuNetworks.empty());
        auto &control = app.model.networks[parent.name];
        auto &live = app.model.networks[child.name];
        control.state = live.state = "Ready";
        control.saslAuthenticated = live.saslAuthenticated = true;
        app.model.servers[1].sojuEnabled = true;
        auto bindSession = [&] {
            const auto &p = app.model.servers[0];
            ServerConfig resolved = app.model.servers[1];
            resolved.host = p.host;
            resolved.port = p.port;
            resolved.tls = p.tls;
            resolved.identity = p.identity;
            resolved.sasl = p.sasl;
            resolved.password = p.password;
            resolved.sojuTrustedFileHost = p.sojuTrustedFileHost;
            control.sojuSessionKey = sojuSessionKey(p, identity);
            live.sojuSessionKey = sojuSessionKey(resolved, identity);
        };
        bindSession();
        Event support;
        support.type = "network";
        support.server = child.name;
        support.tags = {{"kind", "isupport"},
                        {"soju.im/FILEHOST", "https://files.bouncer.example/upload"}};
        app.reduce(support);
        const auto source = sojuFileHostContext(app.model, child.name);
        assert(source && source->account == "account" && source->password == "private");
        live.fileHost = "https://bouncer.example.evil/upload";
        assert(!sojuFileHostContext(app.model, child.name));
        live.fileHost = "https://user:pass@bouncer.example/upload";
        assert(!sojuFileHostContext(app.model, child.name));
        live.fileHost = "http://bouncer.example/upload";
        assert(!sojuFileHostContext(app.model, child.name));
        live.fileHost = source->endpoint;
        assert(sojuFileHostContextCurrent(app.model, *source));
        ++control.sessionGeneration;
        assert(!sojuFileHostContextCurrent(app.model, *source));
        ++app.model.servers[0].port;
        assert(!sojuFileHostContext(app.model, child.name));
        --app.model.servers[0].port;
        app.model.servers[0].sojuTrustedFileHost = "uploads.example";
        live.fileHost = "https://uploads.example/upload";
        assert(!sojuFileHostContext(app.model, child.name)); // Config edit is not a new session.
        bindSession();
        assert(sojuFileHostContext(app.model, child.name));
        live.fileHost = "https://child.uploads.example/upload";
        assert(!sojuFileHostContext(app.model, child.name));
        app.model.servers[0].host = "::1";
        bindSession();
        live.fileHost = "https://[::1]/upload";
        assert(sojuFileHostContext(app.model, child.name));
        live.fileHost = "https://sub.127.0.0.1/upload";
        assert(!sojuFileHostContext(app.model, child.name));
        app.model.servers[0].host = "bouncer.example";
        bindSession();
        live.fileHost = "https://@bouncer.example/upload";
        assert(!sojuFileHostContext(app.model, child.name));
        support.tags = {{"kind", "isupport"}};
        app.reduce(support);
        app.action({"soju-set-enabled", child.name, "0", {}});
        assert(!app.model.servers[1].sojuEnabled && live.state == "Disconnected");
        assert(control.sojuNetworks.contains("42") && app.model.servers.size() == 2);
        assert(live.fileHost.empty());
        Event batch;
        batch.type = "network";
        batch.server = parent.name;
        batch.id = "catalog-session";
        batch.tags = {{"kind", "bouncer-list"}, {"phase", "begin"}};
        app.reduce(batch);
        assert(!control.sojuDiscoveryComplete);
        batch.tags["phase"] = "end";
        batch.id = "stale-batch";
        app.reduce(batch);
        assert(!control.sojuDiscoveryComplete && control.sojuNetworks.contains("42"));
        batch.id = "catalog-session";
        app.reduce(batch);
        assert(control.sojuDiscoveryComplete && control.sojuNetworks.empty());
        assert(app.model.servers.size() == 1 && !app.model.networks.contains(child.name));
        assert(room.draft == "retain draft" && room.messages.front().id == "retained");
        assert(room.readMarker == "2026-01-01T00:00:00Z");
        row.server = parent.name;
        row.id = "42";
        row.tags = {{"kind", "bouncer"}, {"name", "Reused ID"}};
        app.reduce(row);
        assert(control.sojuNetworks.at("42").attributes.at("name") == "Reused ID");
        assert(room.draft == "retain draft");
        Event connecting;
        connecting.type = "state";
        connecting.server = parent.name;
        connecting.text = "Connecting";
        app.reduce(connecting);
        assert(!control.sojuDiscoveryComplete && !control.saslAuthenticated &&
               control.fileHost.empty());
        ServerConfig duplicate = parent;
        duplicate.name = "ambiguous-parent";
        app.model.servers = {parent, duplicate, child};
        app.adoptSojuProfiles();
        assert(app.model.servers[2].sojuParent.empty());
        ServerConfig sameChild = child;
        sameChild.name = "same-bound-id";
        app.model.servers = {parent, child, sameChild};
        app.adoptSojuProfiles();
        assert(app.model.servers[1].sojuParent.empty() && app.model.servers[2].sojuParent.empty());
        const size_t clients = app.clients_.size();
        app.action({"open-server-log", parent.name, {}, {}});
        assert(app.model.activeBuffer == parent.name + "/*server*");
        assert(app.clients_.size() == clients);
    }
    static void sendStatuses() {
        Application app;
        app.model = Model{};
        app.messageCount_ = 0;
        app.activity_.clear();
        app.savingAllowed_ = false;
        app.model.now = 100;
        app.model.settings.retentionDays = 0;
        app.model.networks["send-status"].nick = "Me";
        app.model.networks["other-send-status"].nick = "Me";
        auto &room = app.buffer("send-status", "#status");
        auto &other = app.buffer("other-send-status", "#status");
        app.model.activeBuffer = room.id;

        Event admission;
        admission.type = "message";
        admission.server = room.server;
        admission.target = room.target;
        admission.nick = "Me";
        admission.timestamp = 1770000000;
        admission.text = "queued submission";
        admission.label = "status-submission";
        admission.own = true;
        admission.tags["pending"] = "1";
        app.reduce(admission);
        assert(room.messages.size() == 1 && room.messages.front().pending &&
               room.messages.front().sendStatus == SendStatus::Queued);
        const string localId = room.messages.front().id;
        assert(localId.starts_with("local:"));

        Event otherAdmission = admission;
        otherAdmission.server = other.server;
        app.reduce(otherAdmission);
        assert(other.messages.size() == 1 &&
               other.messages.front().sendStatus == SendStatus::Queued);

        // Time only ends the visual pending hint, never proves a socket write.
        app.tick(102);
        assert(!room.messages.front().pending &&
               room.messages.front().sendStatus == SendStatus::Queued);
        app.tick(132);
        assert(!app.pending_.contains(room.id + "\n" + localId) &&
               room.messages.front().sendStatus == SendStatus::Queued);

        Event sent;
        sent.type = "sent";
        sent.server = room.server;
        sent.target = room.target;
        sent.label = "unrelated-label";
        const auto revision = room.messagesRevision;
        app.reduce(sent);
        sent.label.clear();
        app.reduce(sent);
        sent.label = admission.label;
        sent.server = "unrelated-server";
        app.reduce(sent);
        assert(room.messages.size() == 1 && room.messages.front().id == localId &&
               room.messages.front().sendStatus == SendStatus::Queued &&
               room.messagesRevision == revision);
        sent.server = room.server;
        app.reduce(sent);
        assert(room.messages.size() == 1 && room.messages.front().sendStatus == SendStatus::Sent &&
               other.messages.front().sendStatus == SendStatus::Queued);

        Event echo = admission;
        echo.tags.clear();
        echo.id = "status-canonical";
        echo.timestamp = 1770000000;
        app.reduce(echo);
        assert(room.messages.size() == 1 && room.messages.front().id == echo.id &&
               !room.messages.front().pending &&
               room.messages.front().sendStatus == SendStatus::Confirmed);
        app.reduce(sent);
        assert(room.messages.size() == 1 && room.messages.front().id == echo.id &&
               room.messages.front().sendStatus == SendStatus::Confirmed);

        // Own server history can confirm by exact label or exact ID.
        admission.label = "history-status-label";
        admission.text = "same history text";
        app.reduce(admission);
        const string historyLocalId = room.messages.back().id;
        Event history = admission;
        history.tags.clear();
        history.history = true;
        history.id = "unmatched-history-status";
        history.label.clear();
        history.timestamp = 1770000001;
        history.time = "2026-02-02T02:40:01.000Z";
        completeOwnedHistory(app, room, history, "history-owner-unmatched");
        assert(room.messages.size() == 3 && room.messages[1].id == historyLocalId &&
               room.messages[1].sendStatus == SendStatus::Queued &&
               room.messages.back().sendStatus == SendStatus::Confirmed);
        history.id = "matched-history-status";
        history.label = admission.label;
        completeOwnedHistory(app, room, history, "history-owner-matched");
        assert(room.messages.size() == 3 && room.messages[1].id == history.id &&
               room.messages[1].sendStatus == SendStatus::Confirmed);
        const auto assertReopenedOwnHistoryOrder = [&](const vector<string> &expected) {
            Storage reopened;
            const auto durable = reopened.history(room.id, -1);
            assertHistoryIds(durable, expected);
            assert(!reopened.message(room.id, "status-canonical"));
            assert(room.messages.size() == expected.size() + 1);
            for (size_t i = 0; i < expected.size(); ++i) {
                const auto &native = room.messages[i + 1];
                const auto &stored = durable[i];
                assert(native.id == expected[i] && stored.id == native.id &&
                       stored.time == native.time && stored.timestamp == native.timestamp &&
                       stored.orderingTimestamp == native.orderingTimestamp &&
                       stored.label == native.label && stored.text == native.text &&
                       stored.storageSequence == native.storageSequence &&
                       stored.historyOrder == native.historyOrder &&
                       stored.sendStatus == SendStatus::Confirmed &&
                       native.sendStatus == SendStatus::Confirmed && stored.own && native.own);
            }
        };
        assertReopenedOwnHistoryOrder({"matched-history-status", "unmatched-history-status"});
        sent.label = admission.label;
        app.reduce(sent);
        assert(room.messages[1].sendStatus == SendStatus::Confirmed);
        admission.label = "history-status-id";
        app.reduce(admission);
        history.id = room.messages.back().id;
        history.label = "history-request-label";
        completeOwnedHistory(app, room, history, "history-owner-exact-id");
        assert(room.messages.size() == 4 && room.messages.back().id == history.id &&
               room.messages.back().label == admission.label &&
               room.messages.back().sendStatus == SendStatus::Confirmed);
        assertReopenedOwnHistoryOrder(
            {"matched-history-status", "unmatched-history-status", history.id});
        // A stale explicit owner must not fall back to a child's valid send label.
        admission.label = "still-queued-after-stale-owner";
        app.reduce(admission);
        const string stalePendingId = room.messages.back().id;
        const auto staleCount = room.messages.size();
        Event stale = history;
        stale.label = admission.label;
        stale.id = "stale-history-must-not-confirm";
        stale.tags["history-owner"] = "retired-history-owner";
        app.reduce(stale);
        assert(room.messages.size() == staleCount && room.messages.back().id == stalePendingId &&
               room.messages.back().sendStatus == SendStatus::Queued &&
               !app.storage_.message(room.id, stale.id));

        admission.label = "failed-status-label";
        admission.text = "failed submission";
        app.reduce(admission);
        room.draft = "newer draft";
        Event failure;
        failure.type = "error";
        failure.server = room.server;
        failure.target = room.target;
        failure.label = admission.label;
        failure.text = "Cannot send to channel";
        failure.tags = {{"send-failed", "1"}, {"original-text", admission.text}};
        app.reduce(failure);
        const string recoveredDraft = admission.text + "\nnewer draft";
        assert(room.draft == recoveredDraft);
        assert(std::none_of(room.messages.begin(), room.messages.end(),
                            [&](const auto &m) { return m.label == failure.label; }));
        // A later submission must not inherit completion of the failed label.
        admission.label = "replacement-status-label";
        app.reduce(admission);
        const auto afterFailureCount = room.messages.size();
        sent.label = failure.label;
        app.reduce(sent);
        assert(room.messages.size() == afterFailureCount && room.draft == recoveredDraft &&
               room.messages.back().label == admission.label &&
               room.messages.back().sendStatus == SendStatus::Queued);

        Event incoming = echo;
        incoming.own = false;
        incoming.nick = "Other";
        incoming.id = "incoming-status";
        incoming.label.clear();
        app.reduce(incoming);
        assert(room.messages.back().id == incoming.id &&
               room.messages.back().sendStatus == SendStatus::None);
        app.clear(&room);
        sent.label = admission.label;
        app.reduce(sent);
        assert(room.messages.empty());

        // Disconnect must recover an unwritten ghost even after its timer expired.
        assert(!other.messages.front().pending &&
               other.messages.front().sendStatus == SendStatus::Queued);
        other.draft = "newer disconnected draft";
        Event disconnected;
        disconnected.type = "state";
        disconnected.server = other.server;
        disconnected.text = "Disconnected";
        app.reduce(disconnected);
        // One failure yields one system feedback row; a late completion must
        // neither duplicate feedback nor resurrect the removed own ghost.
        assert(other.messages.size() == 1 && !other.messages.front().own &&
               other.messages.front().presence &&
               other.messages.front().sendStatus == SendStatus::None &&
               other.messages.front().activity == ActivityKind::None &&
               other.draft == otherAdmission.text + "\nnewer disconnected draft");
        sent.server = other.server;
        sent.label = otherAdmission.label;
        app.reduce(sent);
        assert(other.messages.size() == 1 && !other.messages.front().own &&
               other.messages.front().presence &&
               other.messages.front().sendStatus == SendStatus::None &&
               other.messages.front().activity == ActivityKind::None &&
               other.draft == otherAdmission.text + "\nnewer disconnected draft");
    }
    static void avatars() {
        Application app;
        app.model = Model{};
        app.savingAllowed_ = false;
        auto &network = app.model.networks["avatars"];
        network.nick = "Me";
        network.state = "Ready";
        network.caps = {"batch", "draft/metadata-2"};
        auto &room = app.buffer("avatars", "#[Room]");
        room.joined = true;
        auto &other = app.buffer("avatars", "#other");
        other.joined = true;
        Event metadata;
        metadata.type = "network";
        metadata.server = "avatars";
        metadata.tags = {{"kind", "metadata"}, {"command", "METADATA"}};
        metadata.params = {"#{ROOM}", "avatar", "*", "https://example.org/avatar/{size}.png"};
        app.reduce(metadata);
        const auto original = room.avatar;
        assert(!original.empty() && room.avatarPath.empty() && other.avatar.empty());
        assert(room.messages.empty());
        const auto buffers = app.model.buffers.size();
        metadata.params[0] = "#unjoined";
        app.reduce(metadata);
        metadata.params[0] = "SomeNick";
        app.reduce(metadata);
        assert(app.model.buffers.size() == buffers);
        metadata.params = {"#[room]", "display-name", "*", "ignored"};
        app.reduce(metadata);
        assert(room.avatar == original);
        metadata.params = {"#[room]", "avatar", "*", "https://example.org/old-history.png"};
        metadata.history = true;
        app.reduce(metadata);
        assert(room.avatar == original);
        metadata.history = false;

        // Completion tokens identify one live buffer/URL, not the recipient of a numeric.
        const auto first = ++app.avatarSequence_;
        app.channelAvatars_[room.id] = {room.avatar, first};
        app.finishAvatar({first, "/trusted-worker-cache/first.png"});
        assert(room.avatarPath == "/trusted-worker-cache/first.png");
        metadata.tags["command"] = "761";
        metadata.target = "Me"; // IRC numeric recipient must never become the channel target.
        metadata.params = {"Me", "#{room}", "avatar", "*", "https://example.org/new.png"};
        app.reduce(metadata);
        assert(room.avatar == metadata.params.back() && room.avatarPath.empty());
        app.finishAvatar({first, "/trusted-worker-cache/stale.png"});
        assert(room.avatarPath.empty() && !app.model.buffers.contains("avatars/me"));

        const auto renamedRequest = ++app.avatarSequence_;
        app.channelAvatars_[room.id] = {room.avatar, renamedRequest};
        const auto oldId = room.id;
        app.renameBuffer(oldId, "#Renamed");
        auto &renamed = app.buffer("avatars", "#renamed");
        assert(!app.model.buffers.contains(oldId));
        assert(app.channelAvatars_.at(renamed.id).request == renamedRequest);
        app.finishAvatar({renamedRequest, "/trusted-worker-cache/renamed.png"});
        assert(renamed.avatarPath == "/trusted-worker-cache/renamed.png");
        app.renameBuffer(renamed.id, "#RENAMED");
        assert(renamed.target == "#RENAMED" && !renamed.avatarPath.empty());

        metadata.tags["command"] = "766";
        metadata.params = {"Me", "#renamed", "other-key", "key not set"};
        app.reduce(metadata);
        assert(!renamed.avatarPath.empty());
        metadata.params[2] = "avatar";
        app.reduce(metadata);
        assert(renamed.avatar.empty() && renamed.avatarPath.empty());
        metadata.tags["command"] = "METADATA";
        metadata.params = {"#renamed", "avatar", "*", original};
        app.reduce(metadata);
        metadata.params.pop_back(); // Notification without Value removes metadata.
        app.reduce(metadata);
        assert(renamed.avatar.empty());
        metadata.params.push_back(original);
        app.reduce(metadata);
        metadata.params.back().clear(); // Empty URL is a set-but-unusable avatar.
        app.reduce(metadata);
        assert(renamed.avatar.empty() && renamed.avatarPath.empty());

        metadata.params.back() = original;
        app.reduce(metadata);
        const auto disabledRequest = ++app.avatarSequence_;
        app.channelAvatars_[renamed.id] = {renamed.avatar, disabledRequest};
        renamed.avatarPath = "/trusted-worker-cache/before-disable.png";
        app.model.settings.avatars = false;
        app.finishAvatar({disabledRequest, "/trusted-worker-cache/disabled.png"});
        app.updateAvatars();
        assert(renamed.avatar == original && renamed.avatarPath.empty() &&
               app.channelAvatars_.empty());
        metadata.params.back() = "file:///etc/passwd";
        app.reduce(metadata);
        app.model.settings.avatars = true;
        app.updateAvatars();
        const auto attempted = app.avatarSequence_;
        assert(renamed.avatarPath.empty() && app.channelAvatars_.at(renamed.id).request == 0);
        app.updateAvatars();
        assert(app.avatarSequence_ == attempted); // Failed/invalid metadata is not retried.

        // A joined source owns metadata when RENAME merges an offline destination.
        metadata.params.back() = original;
        app.reduce(metadata);
        const auto mergeRequest = ++app.avatarSequence_;
        app.channelAvatars_[renamed.id] = {renamed.avatar, mergeRequest};
        auto &offline = app.buffer("avatars", "#destination");
        offline.avatar = "https://example.org/obsolete.png";
        offline.avatarPath = "/trusted-worker-cache/obsolete.png";
        app.renameBuffer(renamed.id, offline.target);
        assert(offline.joined && offline.avatar == original && offline.avatarPath.empty());
        app.finishAvatar({mergeRequest, "/trusted-worker-cache/merged.png"});
        assert(offline.avatarPath == "/trusted-worker-cache/merged.png");
        Event part;
        part.type = "part";
        part.server = "avatars";
        part.target = offline.target;
        part.nick = "Me";
        app.reduce(part);
        assert(!offline.joined && offline.avatar.empty() && offline.avatarPath.empty());
        metadata.params = {offline.target, "avatar", "*", original};
        app.reduce(metadata);
        assert(offline.avatar.empty()); // Unjoined channels cannot trigger remote loading.
        offline.joined = true;
        app.reduce(metadata);
        Event caps;
        caps.type = "caps";
        caps.server = "avatars";
        const auto batchLostRequest = ++app.avatarSequence_;
        app.channelAvatars_[offline.id] = {offline.avatar, batchLostRequest};
        offline.avatarPath = "/trusted-worker-cache/before-batch-loss.png";
        caps.params = {"draft/metadata-2"};
        app.reduce(caps);
        assert(offline.avatar.empty() && offline.avatarPath.empty());
        app.reduce(metadata);
        assert(offline.avatar.empty()); // Metadata without its batch dependency is ignored.
        app.finishAvatar({batchLostRequest, "/trusted-worker-cache/after-batch-loss.png"});
        assert(offline.avatarPath.empty());
        offline.avatar = original;
        app.channelAvatars_[offline.id] = {offline.avatar, ++app.avatarSequence_};
        app.finishAvatar({app.channelAvatars_.at(offline.id).request,
                          "/trusted-worker-cache/missing-batch.png"});
        assert(offline.avatarPath.empty());
        app.updateAvatars();
        assert(offline.avatarPath.empty() && !app.channelAvatars_.contains(offline.id));
        caps.params = {"batch", "draft/metadata-2"};
        app.reduce(caps);
        app.reduce(metadata);
        caps.params = {"batch"};
        app.reduce(caps);
        assert(offline.avatar.empty() && offline.avatarPath.empty());
        app.reduce(metadata);
        assert(offline.avatar.empty()); // An unnegotiated metadata message is ignored.
        caps.params.push_back("draft/metadata-2");
        app.reduce(caps);
        app.reduce(metadata);
        Event disconnected;
        disconnected.type = "state";
        disconnected.server = "avatars";
        disconnected.text = "Disconnected";
        app.reduce(disconnected);
        assert(!offline.joined && offline.avatar.empty() && offline.avatarPath.empty());

        offline.joined = true;
        app.reduce(metadata);
        const auto closedRequest = ++app.avatarSequence_;
        const auto closedId = offline.id, closedTarget = offline.target;
        app.channelAvatars_[closedId] = {offline.avatar, closedRequest};
        app.action({"close_buffer", closedId, {}, {}});
        auto &reopened = app.buffer("avatars", closedTarget);
        reopened.joined = true;
        app.reduce(metadata);
        app.finishAvatar({closedRequest, "/trusted-worker-cache/closed.png"});
        assert(reopened.avatar == original && reopened.avatarPath.empty());
    }
    static void messageRevisions() {
        RenameDrainPeer peer;
        Application app;
        app.model = Model{};
        app.savingAllowed_ = false;
        app.messageCount_ = 0;
        app.model.now = 100;
        app.model.settings.retentionDays = 0;
        app.storage_.prune(app.model.settings); // Keep fixed fixture timestamps on disk too.
        app.model.settings.typingSend = false;
        Identity identity;
        identity.nick = "Me";
        ServerConfig server;
        server.name = "message-revisions";
        server.host = "127.0.0.1";
        server.port = peer.port;
        server.tls = false;
        app.model.identities = {identity};
        app.model.servers = {server};
        app.clients_[server.name] = std::make_unique<NetworkClient>(server, identity);
        auto &client = *app.clients_.at(server.name);
        client.start();
        peer.accept();
        peer.expect("CAP", {"LS", "302"});
        peer.send(":server CAP * LS :message-tags");
        peer.expect("NICK", {identity.nick});
        peer.expect("USER", {identity.username, "0", "*", identity.realname});
        peer.expect("CAP", {"REQ", "message-tags"});
        peer.send(":server CAP Me ACK :message-tags");
        peer.expect("CAP", {"END"});
        peer.send(":server 001 Me :Welcome");
        peer.send(":server 376 Me :End of MOTD");
        const auto deadline = RenameDrainPeer::Clock::now() + std::chrono::seconds(3);
        while (app.model.networks[server.name].state != "Ready") {
            for (const auto &event : client.poll()) {
                if (event.type == "state" &&
                    (event.text == "Failed" || event.text == "Disconnected"))
                    throw std::runtime_error("Message-revision registration " + event.text);
                app.reduce(event);
            }
            RenameDrainPeer::require(RenameDrainPeer::Clock::now() < deadline,
                                     "Message-revision registration deadline expired");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        app.clear(nullptr);

        auto &source = app.buffer("message-revisions", "#search");
        const auto createdRevision = source.messagesRevision;
        assert(createdRevision > 0);
        Message row;
        const char *texts[] = {"alpha beta", "alpha", "beta", "alpha beta"};
        for (size_t i = 0; i < 4; ++i) {
            row.id = "search-" + std::to_string(i);
            row.text = texts[i];
            app.insert(source, row, false);
        }
        assert(source.messages.size() == 4 && source.messagesRevision == createdRevision);
        app.action({"search", source.id, "alpha", {}});
        auto &results = app.buffer(source.server, "*search*", "search");
        assert(results.messages.size() == 3);
        const auto searchRevision = results.messagesRevision;
        const string searchFirst = results.messages.front().id,
                     searchLast = results.messages.back().id;
        const string searchMiddle = results.messages[1].id;
        // Refresh replaces only the interior identity; count and endpoint cache keys collide.
        assert(results.searchSource == source.id);
        app.action({"search", results.id, "beta", {}});
        assert(results.messages.size() == 3 && results.messages.front().id == searchFirst &&
               results.messages.back().id == searchLast);
        assert(results.messages[1].id == "search:#search:search-2" &&
               results.messages[1].id != searchMiddle && results.messagesRevision > searchRevision);
        const auto sourceRevision = source.messagesRevision;
        app.clear(&source);
        assert(source.messages.empty() && source.messagesRevision > sourceRevision &&
               app.messageCount_ == 0);

        auto &history = app.buffer(server.name, "#history");
        const auto historyCreatedRevision = history.messagesRevision;
        Event playback;
        playback.type = "message";
        playback.server = history.server;
        playback.target = history.target;
        playback.nick = "Other";
        playback.text = "history row";
        playback.history = true;
        playback.id = "history-2";
        playback.timestamp = 2;
        app.reduce(playback);
        playback.id = "history-4";
        playback.timestamp = 4;
        app.reduce(playback);
        assert(history.messages.size() == 2 && history.messages.front().id == "history-2" &&
               history.messages.back().id == "history-4" &&
               history.messagesRevision == historyCreatedRevision);
        playback.id = "history-1";
        playback.timestamp = 1;
        app.reduce(playback);
        assert(history.messages.size() == 3 && history.messages.front().id == "history-1" &&
               history.messages.back().id == "history-4" &&
               history.messagesRevision > historyCreatedRevision);
        const auto prependRevision = history.messagesRevision;
        playback.id = "history-3";
        playback.timestamp = 3;
        app.reduce(playback);
        assert(history.messages.size() == 4 && history.messages.front().id == "history-1" &&
               history.messages.back().id == "history-4" && history.messages[2].id == "history-3" &&
               history.messagesRevision > prependRevision);
        app.clear(&history);

        auto &echoRoom = app.buffer("message-revisions", "EchoPeer", "query");
        row.id = "echo-first";
        row.text = "first";
        row.timestamp = 1;
        app.insert(echoRoom, row, false);
        Event pending;
        pending.type = "message";
        pending.server = echoRoom.server;
        pending.target = echoRoom.target;
        pending.nick = "Me";
        pending.text = "interior pending echo";
        pending.label = "interior-echo-label";
        pending.timestamp = 2;
        pending.own = true;
        pending.tags["pending"] = "1";
        app.reduce(pending);
        row.id = "echo-last";
        row.text = "last";
        row.timestamp = 3;
        app.insert(echoRoom, row, false);
        assert(echoRoom.messages.size() == 3 && echoRoom.messages[1].pending);
        const string pendingId = echoRoom.messages[1].id;
        const string originalLabel = echoRoom.messages[1].label;
        assert(originalLabel == pending.label);
        assert(pendingId.starts_with("local:"));
        const auto labelRevision = echoRoom.messagesRevision;
        assert(app.pending_.contains(echoRoom.id + "\n" + pendingId));
        Event relabeled = pending;
        relabeled.tags.clear();
        relabeled.id = pendingId;
        relabeled.label = "rebound-echo-label";
        app.reduce(relabeled); // Pending-map membership matches by ID, not the changed label.
        assert(echoRoom.messages.size() == 3 && echoRoom.messages.front().id == "echo-first" &&
               echoRoom.messages.back().id == "echo-last" && echoRoom.messages[1].id == pendingId &&
               echoRoom.messages[1].label == relabeled.label &&
               echoRoom.messages[1].label != originalLabel && !echoRoom.messages[1].pending &&
               echoRoom.messagesRevision > labelRevision &&
               !app.pending_.contains(echoRoom.id + "\n" + pendingId));
        const string pendingLabel = echoRoom.messages[1].label;
        const auto echoRevision = echoRoom.messagesRevision;
        Event confirmed = relabeled;
        confirmed.id = "interior-canonical-id";
        app.reduce(confirmed);
        assert(echoRoom.messages.size() == 3 && echoRoom.messages.front().id == "echo-first" &&
               echoRoom.messages.back().id == "echo-last");
        assert(echoRoom.messages[1].id == confirmed.id && confirmed.id != pendingId &&
               !echoRoom.messages[1].pending && echoRoom.messagesRevision > echoRevision);
        const auto &confirmedRow = echoRoom.messages[1];
        assert(confirmedRow.label == pendingLabel); // Canonical label key survives the ID change.
        app.action({"send", echoRoom.id, "reply after confirmation", confirmedRow.id});
        const auto reply = peer.read();
        assert(reply.command == "PRIVMSG" &&
               reply.params == (vector<string>{echoRoom.target, "reply after confirmation"}) &&
               reply.tags.at("+reply") == confirmed.id && reply.tags.at("+reply") != pendingId);
        app.action({"react", echoRoom.id, "+1", confirmedRow.id});
        const auto reaction = peer.read();
        assert(reaction.command == "TAGMSG" && reaction.params == vector<string>{echoRoom.target} &&
               reaction.tags.at("+draft/react") == "+1" &&
               reaction.tags.at("+reply") == confirmed.id &&
               reaction.tags.at("+reply") != pendingId);

        // Rejected Core actions must neither mutate the row nor enqueue a TAGMSG.
        // A subsequent real send is the FIFO witness, avoiding socket-idle timing checks.
        auto rejectedReaction = [&](const string &bufferId, const string &messageId,
                                    bool reconnect = false) {
            const auto reactions = echoRoom.messages[1].reactions;
            const auto revision = echoRoom.messagesRevision;
            app.action({"react", bufferId, "+1", messageId});
            assert(echoRoom.messages[1].reactions == reactions &&
                   echoRoom.messagesRevision == revision);
            if (reconnect)
                app.model.networks.at(server.name).state = "Ready";
            app.action({"send", echoRoom.id, "reaction rejection fence", {}});
            const auto marker = peer.read();
            assert(marker.command == "PRIVMSG" &&
                   marker.params == (vector<string>{echoRoom.target, "reaction rejection fence"}) &&
                   !marker.tags.contains("+draft/react") &&
                   !marker.tags.contains("+draft/unreact"));
        };
        auto &network = app.model.networks.at(server.name);
        const auto negotiatedCaps = network.caps;
        assert(negotiatedCaps.contains("message-tags"));
        network.caps.clear();
        rejectedReaction(echoRoom.id, confirmed.id);
        network.caps = negotiatedCaps;
        for (const auto *deny : {"reply", "draft/react", "*", "*,-draft/react"}) {
            network.clientTagDeny = deny;
            rejectedReaction(echoRoom.id, confirmed.id);
        }
        echoRoom.messages[1].reactions["+1"] = {"mE"};
        for (const auto *deny : {"draft/unreact", "reply", "*,-draft/unreact"}) {
            network.clientTagDeny = deny;
            rejectedReaction(echoRoom.id, confirmed.id);
        }
        network.clientTagDeny.clear();
        echoRoom.messages[1].reactions.clear();
        rejectedReaction(echoRoom.id, "missing-server-id");
        rejectedReaction(echoRoom.id, pendingId); // The superseded local identity is stale.
        auto &diagnostics = app.buffer(server.name, "*server*", "server");
        assert(diagnostics.messages.size() == 1 && diagnostics.messages.front().presence &&
               !diagnostics.messages.front().own);
        // Keep this rejection diagnostic out of the later global two-message retention fixture.
        app.clear(&diagnostics);
        echoRoom.messages[1].deleted = true;
        rejectedReaction(echoRoom.id, confirmed.id);
        echoRoom.messages[1].deleted = false;
        echoRoom.messages[1].pending = true;
        rejectedReaction(echoRoom.id, confirmed.id);
        echoRoom.messages[1].pending = false;
        echoRoom.messages[1].presence = true;
        rejectedReaction(echoRoom.id, confirmed.id);
        echoRoom.messages[1].presence = false;
        rejectedReaction(results.id, results.messages.front().id);

        const auto connectedState = network.state;
        assert(connectedState == "Ready");
        network.state = "Disconnected"; // Retained capabilities do not authorize a send.
        rejectedReaction(echoRoom.id, confirmed.id, true);

        // Reloading an own reaction from disk must select unreact even when the
        // fresh transport has never seen this message/reaction pair.
        const Message canonicalRow = echoRoom.messages[1];
        Message restored = canonicalRow;
        restored.id = "restored-own-reaction";
        restored.reactions["+1"] = {"mE"};
        app.storage_.append(echoRoom.id, restored);
        const auto reloaded = app.storage_.message(echoRoom.id, restored.id);
        RenameDrainPeer::require(reloaded.has_value(), "Restored reaction fixture row is missing");
        echoRoom.messages[1] = *reloaded;
        network.clientTagDeny = "draft/unreact";
        rejectedReaction(echoRoom.id, restored.id);
        network.clientTagDeny = "draft/react";
        app.action({"react", echoRoom.id, "+1", restored.id});
        const auto unreact = peer.read();
        assert(unreact.command == "TAGMSG" && unreact.params == vector<string>{echoRoom.target} &&
               unreact.tags.at("+draft/unreact") == "+1" &&
               unreact.tags.at("+reply") == restored.id && !unreact.tags.contains("+draft/react"));
        network.clientTagDeny.clear();
        echoRoom.messages[1] = canonicalRow;

        // Unknown CASEMAPPING retains RFC1459 identity semantics; ASCII does not.
        // Denying the opposite operation proves Core checks the selected tag.
        const string actualNick = network.nick;
        network.nick = "Me[";
        Event mapping;
        mapping.type = "network";
        mapping.server = server.name;
        for (const bool ascii : {false, true}) {
            mapping.tags = {{"kind", "isupport"},
                            {"CASEMAPPING", ascii ? "ascii" : "unknown-server-mapping"},
                            {"CLIENTTAGDENY", ascii ? "draft/unreact" : "draft/react"}};
            app.reduce(mapping);
            assert(network.caseMapping == (ascii ? "ascii" : "unknown-server-mapping"));
            echoRoom.messages[1] = canonicalRow;
            echoRoom.messages[1].id = ascii ? "ascii-own-reaction" : "unknown-own-reaction";
            echoRoom.messages[1].reactions["+1"] = {"Me{"};
            const string messageId = echoRoom.messages[1].id;
            app.action({"react", echoRoom.id, "+1", messageId});
            const auto selectedReaction = peer.read();
            assert(selectedReaction.command == "TAGMSG" &&
                   selectedReaction.params == vector<string>{echoRoom.target} &&
                   selectedReaction.tags.at("+reply") == messageId &&
                   selectedReaction.tags.at(ascii ? "+draft/react" : "+draft/unreact") == "+1" &&
                   !selectedReaction.tags.contains(ascii ? "+draft/unreact" : "+draft/react"));
        }
        mapping.tags = {{"kind", "isupport"}};
        app.reduce(mapping);
        network.nick = actualNick;
        echoRoom.messages[1] = canonicalRow;

        const auto trimRevision = echoRoom.messagesRevision;
        app.model.settings.maxMessages = 2;
        app.trim();
        assert(echoRoom.messages.size() == 2 && echoRoom.messages.front().id == confirmed.id &&
               echoRoom.messages.back().id == "echo-last" &&
               echoRoom.messagesRevision > trimRevision && app.messageCount_ == 2);
        const auto clearRevision = echoRoom.messagesRevision;
        app.clear(&echoRoom);
        assert(echoRoom.messages.empty() && echoRoom.messagesRevision > clearRevision);
        const string closedId = echoRoom.id, closedTarget = echoRoom.target;
        const auto closedRevision = echoRoom.messagesRevision;
        app.action({"close_buffer", closedId, {}, {}});
        assert(!app.model.buffers.contains(closedId));
        auto &reopened = app.buffer("message-revisions", closedTarget, "query");
        assert(reopened.id == closedId && reopened.messages.empty() &&
               reopened.messagesRevision > closedRevision);

        auto &mergeSource = app.buffer("message-revisions", "#old");
        auto &destination = app.buffer("message-revisions", "#new");
        const int64_t timestamps[] = {1, 0, 2, 4};
        for (size_t i = 0; i < 4; ++i) {
            row.id = "merge-" + std::to_string(i);
            row.timestamp = timestamps[i];
            app.insert(destination, row, false);
        }
        row.id = "merge-1";
        row.timestamp = 3;
        app.insert(mergeSource, row, false);
        const string oldId = mergeSource.id;
        const auto mergeSourceRevision = mergeSource.messagesRevision;
        const auto destinationRevision = destination.messagesRevision;
        assert(destination.messages[1].id == "merge-1" && destination.messages[2].id == "merge-2");
        // A duplicate gains its missing timestamp, reordering only the destination's interior.
        app.renameBuffer(oldId, destination.target);
        assert(!app.model.buffers.contains(oldId) && destination.messages.size() == 4 &&
               destination.messages.front().id == "merge-0" &&
               destination.messages.back().id == "merge-3");
        assert(destination.messages[1].id == "merge-2" && destination.messages[2].id == "merge-1" &&
               destination.messages[2].timestamp == 3 &&
               destination.messagesRevision > destinationRevision &&
               destination.messagesRevision > mergeSourceRevision);
    }
    static void renameDuringDisconnect(bool disconnectCommand) {
        RenameDrainPeer peer;
        Application app; // Stop/join the real worker before closing the peer, including on failure.
        app.model = Model{};
        app.savingAllowed_ = false;
        Identity identity;
        identity.nick = "Me";
        ServerConfig server;
        server.name = disconnectCommand ? "rename-drain-command" : "rename-drain-action";
        server.host = "127.0.0.1";
        server.port = peer.port;
        server.tls = false;
        app.model.identities = {identity};
        app.model.servers = {server};
        app.clients_[server.name] = std::make_unique<NetworkClient>(server, identity);
        auto &client = *app.clients_.at(server.name);
        client.start();
        peer.accept();
        peer.expect("CAP", {"LS", "302"});
        peer.send(":server CAP * LS :");
        peer.expect("NICK", {identity.nick});
        peer.expect("USER", {identity.username, "0", "*", identity.realname});
        peer.expect("CAP", {"END"});
        peer.send(":server 001 Me :Welcome");
        peer.send(":server 376 Me :End of MOTD");
        const auto deadline = RenameDrainPeer::Clock::now() + std::chrono::seconds(3);
        while (app.model.networks[server.name].state != "Ready") {
            for (const auto &event : client.poll()) {
                if (event.type == "state" &&
                    (event.text == "Failed" || event.text == "Disconnected")) {
                    const auto error = event.tags.find("error");
                    throw std::runtime_error(
                        "Rename-drain registration " + event.text +
                        (error == event.tags.end() ? "" : ": " + error->second));
                }
                app.reduce(event);
            }
            RenameDrainPeer::require(RenameDrainPeer::Clock::now() < deadline,
                                     "Rename-drain registration deadline expired");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        peer.send(":Me!user@localhost JOIN #old");
        peer.expect("WHO", {"#old"}); // Own JOIN queries membership before the PING response.
        peer.fence();
        for (const auto &event : client.poll())
            app.reduce(event);
        const string oldId = server.name + "/#old", newId = server.name + "/#new";
        auto &source = app.model.buffers.at(oldId);
        assert(source.joined);
        const string original = "  queued draft 猫✨\nsecond line\t  ";
        source.draft = original;
        app.model.actions = {{disconnectCommand ? "command" : "disconnect",
                              oldId,
                              disconnectCommand ? "/disconnect" : server.name,
                              {}},
                             {"send", oldId, source.draft, {}}};
        source.draft.clear(); // Composer transfers the submission to the queued action.
        bool renamed = false;
        app.model.bufferRenamed = [&](const string &from, const string &to) {
            assert(!renamed && from == oldId && to == newId);
            assert(app.model.actions.empty()); // The remaining send is already in the active batch.
            assert(!app.model.buffers.contains(oldId));
            assert(app.model.buffers.at(newId).draft.empty());
            renamed = true;
        };
        peer.send(":Operator!user@localhost RENAME #old #new :Moved during disconnect");
        peer.fence(); // Real PONG proves RENAME was queued; do not poll it before dispatch.
        assert(!renamed && app.model.buffers.contains(oldId) &&
               !app.model.buffers.contains(newId) && source.draft.empty());

        // Bypass only tick's initial poll: disconnect must stop/drain RENAME inside this batch.
        app.dispatchActions();
        assert(renamed && !app.model.buffers.contains(oldId));
        const auto &destination = app.model.buffers.at(newId);
        assert(destination.target == "#new" && destination.draft == original);
        assert(app.model.networks.at(server.name).state == "Disconnected" && !destination.joined);
        assert(app.model.status ==
               "Not connected to " + server.name); // Not a closed-buffer rejection.
        assert(app.model.actions.empty() && app.queuedSends_.empty() && app.pending_.empty());
        app.model.bufferRenamed = {};
    }
    static void presenceInheritance() {
        Application app;
        app.model = Model{};
        app.savingAllowed_ = false;
        app.model.settings.retentionDays = 0;
        app.model.networks["presence-a"].nick = "Me";
        app.model.networks["presence-b"].nick = "Me";
        auto &a = app.buffer("presence-a", "#chan");
        auto &b = app.buffer("presence-b", "#chan");
        app.model.settings.channelPresence["#chan"] = PresenceMode::Hidden;
        app.model.settings.channelPresence[a.id] = PresenceMode::Global;
        int sequence = 0;
        auto join = [&](Buffer &room, size_t expectedRows) {
            Event event;
            event.type = "join";
            event.server = room.server;
            event.target = room.target;
            event.id = "presence-inheritance-" + std::to_string(++sequence);
            event.nick = "Quiet" + std::to_string(sequence);
            event.timestamp = 1770000000 + sequence;
            app.reduce(event);
            assert(room.messages.size() == expectedRows);
            assert(std::any_of(room.nicks.begin(), room.nicks.end(),
                               [&](const auto &nick) { return nick.name == event.nick; }));
            if (!room.messages.empty() && room.messages.back().id == event.id)
                assert(room.messages.back().presence &&
                       room.messages.back().activity == ActivityKind::Join);
        };
        // An explicit ID-level Global bypasses the shared target override;
        // the same target on another network still inherits Hidden.
        app.model.settings.presence = PresenceMode::All;
        join(a, 1);
        join(b, 0);
        app.model.settings.presence = PresenceMode::Hidden;
        join(a, 1);
        join(b, 0);
        app.model.settings.presence = PresenceMode::Smart;
        join(a, 2); // Smart keeps truthful raw activity even for a quiet newcomer.
        join(b, 0);
        app.model.settings.presence = PresenceMode::All;
        join(a, 3); // Global remains inherited after future preference changes.
        join(b, 0);
        // Concrete ID overrides win too, independently of global and target.
        app.model.settings.channelPresence[b.id] = PresenceMode::All;
        join(b, 1);
        app.model.settings.channelPresence[b.id] = PresenceMode::Smart;
        join(b, 2);
        app.model.settings.channelPresence[b.id] = PresenceMode::Hidden;
        join(b, 2);
        app.model.settings.channelPresence.erase(b.id);
        join(b, 2); // Removing an ID reveals the still-shared Hidden override.
        app.model.settings.channelPresence.erase("#chan");
        join(b, 3); // Without either override the global All setting applies.
    }
    static void chatActivityPins() {
        Application app;
        app.model = Model{};
        app.messageCount_ = 0;
        app.model.settings.retentionDays = 0;
        app.storage_.prune(app.model.settings); // Keep fixed fixture timestamps on disk too.
        app.storage_.saveSettings(
            app.model.settings); // Restart must use the same retention policy.
        app.model.networks["chat-order"].nick = "Me";
        auto &room = app.buffer("chat-order", "#room");
        auto &query = app.buffer(room.server, "Friend", "query");
        auto &utility = app.buffer(room.server, "*server*", "server");
        Event chat;
        chat.type = "message";
        chat.server = room.server;
        chat.target = room.target;
        chat.nick = "Speaker";
        chat.text = "actual chat";
        chat.id = "shared-source-id";
        chat.timestamp = 1700000000;
        chat.time = "2023-11-14T22:13:20.100000001Z";
        app.reduce(chat);
        assert(room.lastChatTimestamp == chat.timestamp && room.lastChatSequence > 0);
        const auto firstSequence = room.lastChatSequence;
        chat.target = query.target;
        chat.time = "2023-11-14T23:13:20.100000001+01:00";
        chat.type = "action";
        chat.nick = "Me";
        chat.own = true;
        app.reduce(chat);
        assert(chatActivityBefore(query, room) && query.messages.back().action &&
               query.messages.back().own);
        const auto storedRoom = app.storage_.message(room.id, chat.id);
        const auto storedQuery = app.storage_.message(query.id, chat.id);
        assert(storedRoom && storedRoom->storageSequence == firstSequence);
        assert(storedQuery && storedQuery->action &&
               storedQuery->storageSequence == query.lastChatSequence &&
               storedQuery->storageSequence > firstSequence);
        const auto latest = query.lastChatSequence;
        // Equal instants (including offset spellings) retain durable tie order on restart.
        {
            Storage restarted;
            Model reloaded;
            restarted.load(reloaded);
            const auto &reloadedRoom = reloaded.buffers.at(room.id);
            const auto &reloadedQuery = reloaded.buffers.at(query.id);
            assert(reloadedRoom.lastChatSequence == firstSequence &&
                   reloadedQuery.lastChatSequence == latest &&
                   reloadedRoom.lastChatTimestamp == reloadedQuery.lastChatTimestamp &&
                   chatActivityBefore(reloadedQuery, reloadedRoom));
        }
        // Replay/backfill can carry later server times but never promote a live chat.
        chat.id = "future-history";
        chat.history = true;
        chat.timestamp += 100;
        app.reduce(chat);
        assert(query.lastChatSequence == latest &&
               query.lastChatTimestamp == room.lastChatTimestamp);
        chat.history = false;
        chat.own = false;
        chat.type = "typing";
        chat.id = "typing-does-not-count";
        app.reduce(chat);
        assert(query.lastChatSequence == latest);
        Message presence;
        presence.id = "presence-does-not-count";
        presence.presence = true;
        presence.timestamp = chat.timestamp + 100;
        assert(app.insert(query, presence));
        assert(query.lastChatSequence == latest);
        chat.type = "message";
        chat.id = "server-does-not-count";
        chat.target = "*server*";
        app.reduce(chat);
        assert(utility.lastChatTimestamp == 0);
        // A repeated durable ID outside the deque is still not a new chat.
        room.messages.clear();
        chat.target = room.target;
        chat.id = "shared-source-id";
        app.reduce(chat);
        assert(room.lastChatSequence == firstSequence && room.lastChatTimestamp == 1700000000);
        const auto replayedRoom = app.storage_.message(room.id, chat.id);
        assert(replayedRoom && replayedRoom->storageSequence == firstSequence);
        room.archived = true;
        app.action({"pin_buffer", room.id, "on", {}});
        app.action({"pin_buffer", query.id, "on", {}});
        app.action({"pin_buffer", room.id, "on", {}});
        const vector<string> pins{query.id, room.id};
        assert(app.model.settings.pinnedBuffers == pins && room.archived);
        app.action({"pin_buffer", "", "on", {}});
        app.action({"pin_buffer", utility.id, "on", {}});
        app.action({"pin_buffer", room.id, "toggle", {}});
        assert(app.model.settings.pinnedBuffers == pins);
        Model restored;
        app.storage_.load(restored);
        assert(restored.settings.pinnedBuffers == pins);
        app.action({"pin_buffer", query.id, "off", {}});
        assert(app.model.settings.pinnedBuffers == vector<string>{room.id} &&
               chatActivityBefore(query, room));
        app.action({"pin_buffer", query.id, "on", {}});
        const string oldId = room.id;
        app.model.settings.channelPresence[oldId] = PresenceMode::Hidden;
        app.model.settings.bufferFools[oldId] = false;
        app.renameBuffer(oldId, "#renamed");
        const string renamed = "chat-order/#renamed";
        assert((app.model.settings.pinnedBuffers == vector<string>{query.id, renamed}));
        assert(app.model.settings.channelPresence.at(renamed) == PresenceMode::Hidden &&
               !app.model.settings.bufferFools.at(renamed));
        app.storage_.load(restored);
        assert(restored.settings.pinnedBuffers == app.model.settings.pinnedBuffers);
        app.renameBuffer(renamed, query.target);
        assert(app.model.settings.pinnedBuffers == vector<string>{query.id} &&
               query.lastChatSequence == latest);
        app.storage_.load(restored);
        assert(restored.settings.pinnedBuffers == vector<string>{query.id});
        const string closed = query.id, target = query.target;
        app.action({"close_buffer", closed, {}, {}});
        app.storage_.load(restored);
        assert(restored.settings.pinnedBuffers.empty() && !restored.buffers.contains(closed));
        auto &reopened = app.buffer("chat-order", target, "query");
        assert(app.model.settings.pinnedBuffers.empty() && reopened.lastChatTimestamp > 0);
        assert(app.model.settings.channelPresence.at(closed) == PresenceMode::Hidden &&
               !app.model.settings.bufferFools.at(closed));
        app.action({"close_buffer", closed, {}, {}});
        app.action({"source-jump", closed, "shared-source-id", {}});
        assert(app.model.buffers.at(closed).lastChatTimestamp == 1700000100 &&
               app.model.settings.pinnedBuffers.empty());

        auto &destination = app.buffer("chat-order", "#duplicate-destination");
        auto &middle = app.buffer("chat-order", "#duplicate-middle");
        auto &source = app.buffer("chat-order", "#duplicate-source");
        Message duplicate;
        duplicate.id = "merge-duplicate";
        duplicate.nick = "Speaker";
        duplicate.text = "same message";
        duplicate.timestamp = 1700000200;
        assert(app.insert(destination, duplicate));
        duplicate.id = "middle-row";
        assert(app.insert(middle, duplicate));
        duplicate.id = "merge-duplicate";
        assert(app.insert(source, duplicate));
        const auto survivorSequence = destination.lastChatSequence;
        assert(chatActivityBefore(source, middle) && chatActivityBefore(middle, destination));
        app.renameBuffer(source.id, destination.target);
        assert(destination.lastChatSequence == survivorSequence &&
               chatActivityBefore(middle, destination));

        auto &echoRoom = app.buffer("chat-order", "#echo-correction");
        auto &between = app.buffer("chat-order", "#echo-between");
        Message pending;
        pending.id = "local:corrected-own";
        pending.label = "corrected-own";
        pending.nick = "Me";
        pending.text = "own pending";
        pending.timestamp = 1700000310;
        pending.own = pending.pending = true;
        assert(app.insert(echoRoom, pending));
        Message other;
        other.id = "between-message";
        other.nick = "Speaker";
        other.timestamp = 1700000305;
        assert(app.insert(between, other));
        assert(chatActivityBefore(echoRoom, between));
        Event echo;
        echo.type = "message";
        echo.server = echoRoom.server;
        echo.target = echoRoom.target;
        echo.id = "confirmed-own";
        echo.label = pending.label;
        echo.text = pending.text;
        echo.nick = "Me";
        echo.own = true;
        echo.timestamp = 1700000300;
        app.reduce(echo);
        assert(echoRoom.lastChatTimestamp == echo.timestamp &&
               chatActivityBefore(between, echoRoom));
        other.id = "preceding-real-chat";
        assert(app.insert(echoRoom, other));
        pending.id = "local:second-correction";
        pending.label = "second-correction";
        pending.timestamp = 1700000320;
        assert(app.insert(echoRoom, pending));
        Message laterReplay = other;
        laterReplay.id = "echo-room-future-replay";
        laterReplay.history = true;
        laterReplay.timestamp = 1700001000;
        assert(app.insert(echoRoom, laterReplay));
        echo.id = "confirmed-second-correction";
        echo.label = pending.label;
        echo.timestamp = 1700000290;
        app.reduce(echo);
        assert(echoRoom.lastChatTimestamp == other.timestamp && echoRoom.lastChatId == other.id);

        auto &historyEchoRoom = app.buffer("chat-order", "#history-own-confirmation");
        pending.id = "local:history-own";
        pending.label = "history-own";
        pending.timestamp = 1700000100;
        assert(app.insert(historyEchoRoom, pending));
        echo.target = historyEchoRoom.target;
        echo.id = "history-confirmed-own";
        echo.label = pending.label;
        echo.history = true;
        echo.timestamp = 1700000200;
        completeOwnedHistory(app, historyEchoRoom, echo, "chat-activity-history-confirmation");
        assert(historyEchoRoom.lastChatTimestamp == pending.timestamp &&
               historyEchoRoom.messages.back().timestamp == echo.timestamp);
        app.renameBuffer(historyEchoRoom.id, "#history-own-renamed");
        auto &historyRenamed = app.buffer("chat-order", "#history-own-renamed");
        assert(historyRenamed.lastChatTimestamp == pending.timestamp);
        app.repairChatActivity(historyRenamed);
        assert(historyRenamed.lastChatTimestamp == pending.timestamp);
        echo.target = historyRenamed.target;
        echo.history = false;
        echo.timestamp = 1700000250;
        app.reduce(echo);
        assert(historyRenamed.lastChatTimestamp == echo.timestamp &&
               historyRenamed.replayChatKeys.empty());
        auto &pureHistory = app.buffer("chat-order", "#pure-own-history");
        Message historicalOwn = pending;
        historicalOwn.id = "pure-historical-own";
        historicalOwn.history = true;
        historicalOwn.pending = false;
        assert(app.insert(pureHistory, historicalOwn));
        echo.target = pureHistory.target;
        echo.id = historicalOwn.id;
        echo.label = historicalOwn.label;
        echo.history = true;
        echo.timestamp = 1700000400;
        completeOwnedHistory(app, pureHistory, echo, "chat-activity-historical-own-replay");
        app.renameBuffer(pureHistory.id, "#pure-own-history-renamed");
        assert(app.buffer("chat-order", "#pure-own-history-renamed").lastChatTimestamp == 0);

        // A later-opened destination must not admit the source's earlier replay rows.
        auto &replaySource = app.buffer("chat-order", "#replay-source");
        Message replay = other;
        replay.id = "later-history";
        replay.history = true;
        replay.timestamp = 1700001000;
        assert(app.insert(replaySource, replay));
        auto &seededDestination = app.buffer("chat-order", "#seeded-destination");
        other.id = "seeded-live";
        other.timestamp = 1700000500;
        assert(app.insert(seededDestination, other));
        const string seededId = seededDestination.id, seededTarget = seededDestination.target;
        app.action({"close_buffer", seededId, {}, {}});
        auto &seeded = app.buffer("chat-order", seededTarget);
        assert(seeded.lastChatTimestamp == other.timestamp);
        app.renameBuffer(replaySource.id, seeded.target);
        assert(seeded.lastChatTimestamp == other.timestamp);
        app.repairChatActivity(seeded);
        assert(seeded.lastChatTimestamp == other.timestamp);

        auto &historyDestination = app.buffer("chat-order", "#history-duplicate");
        replay.id = "history-then-live-merge";
        assert(app.insert(historyDestination, replay));
        const auto historicalSequence = historyDestination.messages.back().storageSequence;
        auto &liveSource = app.buffer("chat-order", "#live-duplicate");
        other.id = replay.id;
        assert(app.insert(liveSource, other));
        app.renameBuffer(liveSource.id, historyDestination.target);
        assert(historyDestination.lastChatSequence == historicalSequence &&
               !historyDestination.chatReplaySequences.contains(historicalSequence));
    }
    static void chatControls() {
        Application app;
        app.model = Model{};
        app.messageCount_ = 0;
        app.model.settings.retentionDays = 0;
        app.model.settings.typingSend = false;
        app.model.now = 100;
        app.model.networks["chat-controls"].nick = "Me";
        auto &room = app.buffer("chat-controls", "#room");
        auto &other = app.buffer(room.server, "Friend", "query");
        auto &utility = app.buffer(room.server, "*server*", "server");
        ServerConfig config;
        config.name = room.server;
        config.channels = {room.target};
        app.model.servers = {config};
        room.joined = true;
        room.draft = "raw unsent draft";
        room.readMarker = "2026-01-01T00:00:00Z";
        const string marker = room.readMarker;
        app.model.activeBuffer = other.id;
        app.model.settings.workspace["visible_buffers"] = room.id;
        app.action({"mute_buffer", room.id, "on", {}});
        assert(room.muted && !other.muted && room.joined && room.draft == "raw unsent draft");
        app.action({"mute_buffer", room.id, "on", {}}); // Explicit sets are idempotent.
        app.action({"mute_buffer", "", "off", {}});
        app.action({"mute_buffer", "missing/chat", "on", {}});
        app.action({"mute_buffer", utility.id, "on", {}});
        app.action({"mute_buffer", room.id, "toggle", {}});
        assert(room.muted && !other.muted && !utility.muted);

        Event incoming;
        incoming.type = "message";
        incoming.server = room.server;
        incoming.target = room.target;
        incoming.nick = "Speaker";
        incoming.text = "Me: retained mention";
        incoming.id = "muted-live";
        incoming.timestamp = 1793836800;
        incoming.time = "2026-11-05T00:00:00Z";
        app.reduce(incoming);
        assert(room.messages.size() == 1 && room.unread == 1 && room.messages.back().mention);
        assert(app.model.effects.size() == 1 && app.model.effects.back().type == "mention" &&
               app.model.effects.back().silent);
        assert(room.readMarker == marker);
        Event ownEffect = incoming;
        ownEffect.type = "send";
        ownEffect.own = true;
        app.effect(ownEffect, room);
        assert(app.model.effects.back().type == "send" && app.model.effects.back().silent);
        app.model.networks[room.server].state = "Ready";
        Event disconnected;
        disconnected.type = "state";
        disconnected.server = room.server;
        disconnected.text = "Disconnected";
        app.reduce(disconnected);
        assert(app.model.effects.back().type == "state" && !app.model.effects.back().silent);
        room.joined = true;
        app.model.effects.clear();
        app.model.activeBuffer = room.id;
        app.action({"archive_buffer", room.id, "on", {}});
        assert(room.archived && room.joined && app.model.activeBuffer == utility.id &&
               room.draft == "raw unsent draft" && room.readMarker == marker &&
               app.model.servers.front().channels == vector<string>{room.target});
        assert(app.storage_.message(room.id, incoming.id));
        Model restored;
        app.storage_.load(restored);
        assert(restored.buffers.at(room.id).muted && restored.buffers.at(room.id).archived &&
               restored.buffers.at(room.id).draft == room.draft &&
               restored.buffers.at(room.id).readMarker == marker &&
               restored.activeBuffer == utility.id);
        incoming.id = "muted-archived-arrival";
        app.reduce(incoming);
        assert(room.archived && room.unread == 2);
        app.action({"mute_buffer", room.id, "off", {}});
        assert(room.archived && !room.muted);
        incoming.history = true;
        incoming.id = "archived-history";
        app.reduce(incoming);
        assert(room.archived);
        incoming.history = false;
        incoming.own = true;
        incoming.nick = "Me";
        incoming.id = "archived-own";
        app.reduce(incoming);
        assert(room.archived);
        incoming.own = false;
        incoming.nick = "Speaker";
        incoming.id = "archived-history"; // A replayed ID is not a new arrival.
        app.reduce(incoming);
        assert(room.archived);
        app.model.settings.fools.insert("Speaker");
        app.model.settings.foolUnread = true;
        incoming.id = "archived-fool";
        app.reduce(incoming);
        assert(room.archived && room.messages.back().id == incoming.id);
        app.model.settings.fools.clear();
        app.model.settings.foolUnread = false;
        // Retained replay outside the in-memory window must not count as new activity.
        Message older;
        older.id = "durable-only";
        older.nick = incoming.nick;
        older.text = incoming.text;
        older.timestamp = incoming.timestamp;
        app.storage_.append(room.id, older);
        incoming.id = older.id;
        app.reduce(incoming);
        assert(room.archived);
        incoming.id = "unmuted-new-arrival";
        app.model.effects.clear();
        app.reduce(incoming);
        assert(!room.archived && room.unread == 6 &&
               std::any_of(app.model.effects.begin(), app.model.effects.end(),
                           [](const auto &e) { return e.type == "notify" && !e.silent; }));
        app.storage_.load(restored);
        assert(!restored.buffers.at(room.id).archived && !restored.buffers.at(room.id).muted &&
               app.storage_.history(room.id, 20).size() == room.messages.size());

        // Queued origin IDs survive a rename; losing an origin must never target selection.
        app.model.actions = {{"mute_buffer", room.id, "on", {}},
                             {"archive_buffer", room.id, "on", {}}};
        const string oldId = room.id;
        app.renameBuffer(oldId, "#renamed");
        app.dispatchActions();
        auto &renamed = app.buffer("chat-controls", "#renamed");
        assert(renamed.muted && renamed.archived && renamed.joined && !other.muted);
        other.archived = false;
        other.muted = false;
        app.renameBuffer(renamed.id, other.target);
        assert(other.muted && !other.archived && other.joined &&
               other.draft == "raw unsent draft" && !other.messages.empty());
        app.model.activeBuffer = other.id;
        app.action({"archive_buffer", oldId, "on", {}});
        assert(!other.archived);
        auto &source = app.buffer("chat-controls", "Second", "query");
        source.archived = true;
        source.muted = false;
        other.archived = true;
        app.renameBuffer(source.id, other.target);
        assert(other.archived && other.muted);
        // The old whole-application save performed a second prune after committing flags.
        // Make that second pass fail: the action must perform only its atomic save.
        Message maintenance;
        maintenance.id = "flag-maintenance-first";
        maintenance.nick = "Fixture";
        maintenance.text = "expired fixture";
        maintenance.timestamp = 1;
        app.storage_.append(other.id, maintenance);
        sqlite3 *fault = nullptr;
        const auto database =
            std::filesystem::path(std::getenv("XDG_DATA_HOME")) / "blastirc/state.sqlite3";
        assert(sqlite3_open(database.c_str(), &fault) == SQLITE_OK);
        assert(
            sqlite3_exec(
                fault,
                "CREATE TRIGGER first_flag_prune AFTER DELETE ON messages "
                "WHEN OLD.id='flag-maintenance-first' BEGIN "
                "INSERT INTO messages(buffer,id,nick,text,time,reply,label,timestamp,received_at,"
                "sort_time,own,pending,deleted,mention,action,presence) VALUES("
                "OLD.buffer,'flag-maintenance-second','Fixture','','','','',1,1,1,0,0,0,0,0,0); "
                "END;"
                "CREATE TRIGGER reject_second_flag_prune BEFORE DELETE ON messages "
                "WHEN OLD.id='flag-maintenance-second' BEGIN "
                "SELECT RAISE(FAIL,'second maintenance pass rejected'); END;",
                nullptr, nullptr, nullptr) == SQLITE_OK);
        app.model.settings.retentionDays = 1;
        app.action({"mute_buffer", other.id, "off", {}});
        assert(!other.muted);
        sqlite3_stmt *persistedFlag = nullptr;
        assert(sqlite3_prepare_v2(fault, "SELECT muted FROM buffers WHERE id=?", -1, &persistedFlag,
                                  nullptr) == SQLITE_OK);
        assert(sqlite3_bind_text(persistedFlag, 1, other.id.c_str(), -1, SQLITE_TRANSIENT) ==
               SQLITE_OK);
        assert(sqlite3_step(persistedFlag) == SQLITE_ROW &&
               sqlite3_column_int(persistedFlag, 0) == 0);
        sqlite3_finalize(persistedFlag);
        assert(!app.save()); // The independently requested maintenance really does fail.
        assert(!other.muted);
        assert(sqlite3_exec(fault,
                            "DROP TRIGGER first_flag_prune;"
                            "DROP TRIGGER reject_second_flag_prune;",
                            nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(fault);
        app.model.settings.retentionDays = 0;
        other.muted = true;
        // Failed persistence must not claim that a requested change succeeded.
        app.savingAllowed_ = false;
        app.action({"mute_buffer", other.id, "off", {}});
        assert(other.muted && !app.model.status.empty());
        app.action({"archive_buffer", other.id, "off", {}});
        assert(other.archived);
        Action send{"send", other.id, "unprefixed recovery", {}};
        send.attributes["reply-nick"] = "Speaker";
        other.draft.clear();
        app.action(send);
        assert(other.draft == "unprefixed recovery");
        // Network rejection metadata already includes the /me command marker.
        other.draft.clear();
        Event action;
        action.type = "action";
        action.server = other.server;
        action.target = other.target;
        action.nick = "Me";
        action.text = "waves";
        action.label = "raw-action-rejection";
        action.own = true;
        action.tags["pending"] = "1";
        app.reduce(action);
        Event failure = action;
        failure.type = "error";
        failure.text = "synthetic action rejection";
        failure.tags = {{"send-failed", "1"}, {"original-text", "/me waves"}};
        app.reduce(failure);
        assert(other.draft == "/me waves");
        // A late multiline downgrade must recover full raw text, not its old wire prefix.
        other.draft.clear();
        Event multiline = action;
        multiline.type = "message";
        multiline.text = "Speaker: first\nsecond";
        multiline.label = "raw-late-retry";
        multiline.tags = {{"pending", "1"},
                          {"submission", multiline.label},
                          {"submission-complete", "1"},
                          {"submission-parts", "1"},
                          {"original-text", "first\nsecond"}};
        app.reduce(multiline);
        multiline.text = "Speaker: first";
        multiline.tags.erase("submission-complete");
        multiline.tags.erase("submission-parts");
        multiline.tags["original-text"] = "first";
        app.reduce(multiline);
        failure.label = multiline.label;
        failure.tags = {{"send-failed", "1"}};
        app.reduce(failure);
        assert(other.draft == "first\nsecond");
    }
    static void foolPolicy() {
        Application app;
        app.model = Model{};
        app.messageCount_ = 0;
        app.model.now = 100;
        app.model.settings.retentionDays = 0;
        app.model.settings.typingSend = false;
        app.model.settings.fools = {"Noisy[", "Caret^"};
        auto &network = app.model.networks["fool-policy"];
        network.nick = "Me";
        auto &room = app.buffer("fool-policy", "#room");
        app.model.activeBuffer = room.id;
        app.model.settings.workspace["visible_buffers"] = room.id;
        assert(isFool(app.model, room.server, "nOISY{") &&
               isFool(app.model, "unknown-network", "CARET~") &&
               !isFool(app.model, room.server, ""));
        Event support;
        support.type = "network";
        support.server = room.server;
        support.tags = {{"kind", "isupport"}, {"CASEMAPPING", "ascii"}};
        app.reduce(support);
        assert(!isFool(app.model, room.server, "Noisy{") &&
               isFool(app.model, room.server, "NOISY["));
        support.tags["CASEMAPPING"] = "strict-rfc1459";
        app.reduce(support);
        assert(isFool(app.model, room.server, "Noisy{") &&
               !isFool(app.model, room.server, "Caret~"));
        support.tags["CASEMAPPING"] = "unknown-mapping";
        app.reduce(support);
        assert(isFool(app.model, room.server, "Caret~"));

        for (const auto style : {FoolDisplay::Minimized, FoolDisplay::Blurred, FoolDisplay::Dimmed,
                                 FoolDisplay::Hidden}) {
            app.model.settings.foolDisplay = style;
            assert(effectiveFoolDisplay(app.model.settings, room.id, room.target) == style);
            app.model.settings.bufferFools[room.target] = true;
            assert(effectiveFoolDisplay(app.model.settings, room.id, room.target) ==
                   FoolDisplay::Shown);
            app.model.settings.bufferFools[room.id] = false;
            assert(effectiveFoolDisplay(app.model.settings, room.id, room.target) ==
                   FoolDisplay::Hidden);
            app.model.settings.bufferFools[room.target] = false;
            app.model.settings.bufferFools[room.id] = true;
            assert(effectiveFoolDisplay(app.model.settings, room.id, room.target) ==
                   FoolDisplay::Shown);
            app.model.settings.bufferFools.clear();
            assert(effectiveFoolDisplay(app.model.settings, room.id, room.target) == style);
        }

        Event message;
        message.type = "message";
        message.server = room.server;
        message.target = room.target;
        message.nick = "Noisy{";
        message.id = "fool-retained";
        message.text = "Me: retained verbatim";
        message.timestamp = 1793836800;
        message.time = "2026-11-05T00:00:00Z";
        app.reduce(message);
        assert(room.messages.size() == 1 && room.messages.back().text == message.text &&
               room.messages.back().mention && room.unread == 0 && app.model.effects.empty());
        const auto stored = app.storage_.message(room.id, message.id);
        assert(stored && stored->text == message.text && stored->nick == message.nick &&
               stored->mention);
        app.refreshMentions();
        const auto &foolMentions = app.model.mentionSources.at(room.id).messages;
        assert(foolMentions.size() == 1 && foolMentions.front().id == message.id &&
               foolMentions.front().mention && app.model.effects.empty());

        Event typing = message;
        typing.type = "typing";
        typing.text = "active";
        app.reduce(typing);
        assert(room.typing.empty());
        typing.nick = "Speaker";
        app.reduce(typing);
        assert(room.typing.contains("Speaker"));
        app.model.settings.fools.insert("speaker");
        app.tick(101);
        assert(room.typing.empty() && app.model.effects.empty());

        message.nick = "Friend";
        message.id = "friend-mention";
        app.reduce(message);
        assert(room.unread == 1 && room.messages.back().mention &&
               std::any_of(app.model.effects.begin(), app.model.effects.end(),
                           [](const auto &event) { return event.type == "notify"; }));
        app.model.effects.clear();
        app.model.settings.foolUnread = true;
        app.tick(102);
        assert(room.unread == 2 && app.model.effects.empty());
        message.nick = "Noisy{";
        message.id = "fool-counted";
        app.reduce(message);
        assert(room.unread == 3 && app.model.effects.empty());
        app.model.settings.foolUnread = false;
        app.tick(103);
        assert(room.unread == 1 && room.readMarker.empty());
        app.model.settings.fools.clear();
        app.tick(104);
        assert(room.unread == 3 && app.model.effects.empty());
        app.model.settings.fools.insert("Noisy[");
        app.tick(105);
        assert(room.unread == 1);

        // Own sends must retain send effects even if our nick is listed globally.
        network.nick = "Noisy{";
        message.id = "fool-own-send";
        message.own = true;
        app.reduce(message);
        assert(room.messages.back().own && room.unread == 1 && !app.model.effects.empty());
        app.model.effects.clear();
        message.own = false;
        network.nick = "Me";
        message.history = true;
        message.timestamp -= 60;
        message.time = "2026-11-04T23:59:00Z";
        message.id = "fool-history";
        app.reduce(message);
        assert(app.model.effects.empty() && room.unread == 1 &&
               std::any_of(room.messages.begin(), room.messages.end(), [](const auto &row) {
                   return row.id == "fool-history" && row.history &&
                          row.text == "Me: retained verbatim";
               }));

        Event renamed = message;
        renamed.history = false;
        renamed.type = "join";
        renamed.nick = "Friend";
        renamed.id = "structured-nick-join";
        renamed.text.clear();
        app.reduce(renamed);
        renamed.type = "nick";
        renamed.id = "structured-nick-change";
        renamed.text = "NewFriend[";
        app.reduce(renamed);
        assert(room.messages.back().activity == ActivityKind::Nick &&
               room.messages.back().activityTarget == renamed.text);
        const auto savedNick = app.storage_.message(room.id, renamed.id);
        assert(savedNick && savedNick->activity == ActivityKind::Nick &&
               savedNick->activityTarget == renamed.text);

        // Muted moderator redactions invalidate participation without producing FX,
        // and remove the old speaker evidence after a structured nick transfer.
        const auto speakerKey = room.id + "\n" + app.fold(room.server, "NewFriend[");
        assert(app.activity_.contains(speakerKey));
        const auto identityRevision = room.messagesRevision;
        const auto participationRevision = room.participationRevision;
        Event redact = message;
        redact.history = false;
        redact.type = "redact";
        redact.nick = "Noisy{";
        redact.id = "friend-mention";
        app.model.effects.clear();
        app.reduce(redact);
        assert(room.participationRevision == participationRevision + 1 &&
               room.messagesRevision == identityRevision && app.model.effects.empty() &&
               !app.activity_.contains(speakerKey));
        const auto deletedSpeaker = app.storage_.message(room.id, redact.id);
        assert(deletedSpeaker && deletedSpeaker->deleted);
        app.reduce(redact);
        assert(room.participationRevision == participationRevision + 1);
    }
    static void retainedMentions() {
        Application app;
        app.model = Model{};
        app.messageCount_ = 0;
        app.model.now = 100;
        app.model.settings.retentionDays = 0;
        app.storage_.prune(app.model.settings);
        app.model.settings.typingSend = false;
        app.model.networks["mentions-one"].nick = "Me";
        app.model.networks["mentions-two"].nick = "Me";
        auto &first = app.buffer("mentions-one", "#room");
        auto &second = app.buffer("mentions-two", "#room");
        app.model.activeBuffer = first.id;
        app.model.settings.workspace["visible_buffers"] = first.id;
        first.draft = "unfinished reply";
        first.readMarker = second.readMarker = "2026-11-04T23:59:00Z";

        Event incoming;
        incoming.type = "message";
        incoming.server = first.server;
        incoming.target = first.target;
        incoming.nick = "Speaker";
        incoming.text = "Me: incoming mention";
        incoming.id = "same-raw-id";
        incoming.timestamp = 1793836800;
        incoming.time = "2026-11-05T00:00:00Z";
        app.reduce(incoming);
        incoming.server = second.server;
        incoming.timestamp += 1;
        incoming.time = "2026-11-05T00:00:01Z";
        app.reduce(incoming);

        incoming.server = first.server;
        incoming.id = "own-mention";
        incoming.own = true;
        app.reduce(incoming);
        incoming.own = false;
        incoming.id = "deleted-mention";
        app.reduce(incoming);
        Event redact = incoming;
        redact.type = "redact";
        app.reduce(redact);
        incoming.id = "not-a-whole-nick";
        incoming.text = "SomeMeSuffix is not your nick";
        app.reduce(incoming);
        assert(app.save());

        // Legacy unclassified mentions outside the loaded tail must be backfilled too.
        for (int i = 0; i < 225; ++i) {
            Message old;
            old.id = "older-mention-" + std::to_string(i);
            old.nick = "Speaker";
            old.text = "hello, mE!";
            old.timestamp = 1793836400 + i;
            app.storage_.append(first.id, old);
        }
        const auto tail = app.storage_.history(first.id, 200);
        app.messageCount_ -= first.messages.size();
        first.messages.assign(tail.begin(), tail.end());
        app.messageCount_ += first.messages.size();
        app.reviseMessages(first);
        assert(first.messages.size() == 200 &&
               std::none_of(first.messages.begin(), first.messages.end(),
                            [](const auto &row) { return row.id == "older-mention-0"; }));
        app.refreshMentions();
        const auto &sources = app.model.mentionSources;
        assert(sources.at(first.id).messages.size() == 226 &&
               sources.at(first.id).messages.front().id == "older-mention-0" &&
               sources.at(first.id).messages.back().id == "same-raw-id" &&
               sources.at(second.id).messages.size() == 1);
        const auto backfilled = app.storage_.message(first.id, "older-mention-0");
        assert(backfilled && backfilled->mention);
        const auto &feed = app.model.buffers.at(GlobalMentionsBufferId);
        const Message *firstRow = nullptr, *secondRow = nullptr;
        for (const auto &row : feed.messages) {
            if (row.sourceBuffer != first.id && row.sourceBuffer != second.id)
                continue;
            assert(row.sourceId != "own-mention" && row.sourceId != "deleted-mention" &&
                   row.sourceId != "not-a-whole-nick");
            if (row.sourceId == "same-raw-id") {
                if (row.sourceBuffer == first.id)
                    firstRow = &row;
                else
                    secondRow = &row;
            }
        }
        assert(firstRow && secondRow && firstRow->id != secondRow->id &&
               firstRow->sourceServer == first.server && secondRow->sourceServer == second.server &&
               firstRow->sourceTarget == first.target && secondRow->sourceTarget == second.target);
        assert(resolveMessageSource(app.model, feed, *firstRow) == &first);
        const auto sourceKind = first.kind;
        first.kind = "search";
        assert(!resolveMessageSource(app.model, feed, *firstRow));
        first.kind = sourceKind;
        for (bool Message::*flag :
             {&Message::own, &Message::presence, &Message::deleted, &Message::mention}) {
            auto forged = *firstRow;
            forged.*flag = !(forged.*flag);
            assert(!resolveMessageSource(app.model, feed, forged));
        }
        const auto *original = sourceMessage(app.model, first.id, firstRow->sourceId);
        assert(original && resolveMessageSource(app.model, first, *original) == &first);
        auto spoofedOriginal = *original;
        spoofedOriginal.sourceId = firstRow->sourceId;
        assert(!resolveMessageSource(app.model, first, spoofedOriginal));
        assert(std::find_if(feed.messages.begin(), feed.messages.end(), [&](const auto &row) {
                   return row.id == secondRow->id;
               }) < std::find_if(feed.messages.begin(), feed.messages.end(), [&](const auto &row) {
                   return row.id == firstRow->id;
               }));

        const auto firstMarker = first.readMarker, secondMarker = second.readMarker;
        const auto firstUnread = first.unread, secondUnread = second.unread;
        app.action({"global-mentions", {}, {}, {}});
        app.model.settings.workspace["visible_buffers"] = GlobalMentionsBufferId;
        app.action({"read", GlobalMentionsBufferId, {}, {}});
        assert(app.model.activeBuffer == GlobalMentionsBufferId && feed.kind == "mentions" &&
               first.readMarker == firstMarker && second.readMarker == secondMarker &&
               first.unread == firstUnread && second.unread == secondUnread);
        const auto saveRequest = ++app.model.settingsSaveRequest;
        app.action({"save", GlobalMentionsBufferId, {}, {}, saveRequest});
        assert(app.model.settingsSaveResult.requestId == saveRequest &&
               app.model.settingsSaveResult.success);

        // A failed redaction save must outrank stale durable history.
        const auto stale = app.storage_.message(first.id, "older-mention-1");
        assert(stale && !stale->deleted &&
               std::none_of(first.messages.begin(), first.messages.end(),
                            [&](const auto &row) { return row.id == stale->id; }));
        auto tombstone = *stale;
        tombstone.deleted = true;
        tombstone.text.clear();
        Buffer unsaved;
        unsaved.id = first.id;
        unsaved.server = first.server;
        unsaved.target = first.target;
        unsaved.kind = first.kind;
        unsaved.messages.push_back(tombstone);
        app.unsavedSources_[first.id] = std::move(unsaved);
        app.reviseMessages(first);
        Event reaction = incoming;
        reaction.type = "reaction";
        reaction.id = stale->id;
        reaction.text = "+1";
        app.reduce(reaction);
        const auto afterReaction = app.storage_.message(first.id, stale->id);
        assert(afterReaction && afterReaction->reactions.empty() &&
               app.unsavedSources_.at(first.id).messages.front().deleted);
        app.refreshMentions();
        assert(std::none_of(feed.messages.begin(), feed.messages.end(), [&](const auto &row) {
            return row.sourceBuffer == first.id && row.sourceId == stale->id;
        }));
        const auto beforeTombstoneJump = app.model.pendingSourceJump.revision;
        app.action({"source-jump", first.id, stale->id, {}});
        assert(app.model.pendingSourceJump.revision == beforeTombstoneJump &&
               app.model.activeBuffer == GlobalMentionsBufferId);
        app.insert(first, *stale, false);
        const auto restored = std::find_if(first.messages.begin(), first.messages.end(),
                                           [&](const auto &row) { return row.id == stale->id; });
        assert(restored != first.messages.end() && restored->deleted && restored->text.empty() &&
               restored->reactions.empty());

        const auto initialRevision = app.model.pendingSourceJump.revision;
        app.action({"source-jump", first.id, "older-mention-0", {}});
        assert(app.model.activeBuffer == first.id && first.draft == "unfinished reply" &&
               app.model.pendingSourceJump.revision == initialRevision + 1 &&
               app.model.pendingSourceJump.buffer == first.id &&
               app.model.pendingSourceJump.message == "older-mention-0" &&
               first.messages.size() == app.storage_.history(first.id, -1).size() &&
               std::count_if(first.messages.begin(), first.messages.end(),
                             [](const auto &row) {
                                 return row.id == "older-mention-0" && row.mention;
                             }) == 1 &&
               second.messages.size() == 1 && second.messages.front().id == "same-raw-id");

        const auto pending = app.model.pendingSourceJump;
        app.action({"source-jump", first.id, "deleted-mention", {}});
        assert(app.model.pendingSourceJump.revision == pending.revision &&
               app.model.pendingSourceJump.buffer == pending.buffer &&
               app.model.pendingSourceJump.message == pending.message);
        app.command(first, "/clear");
        assert(first.messages.empty());
        assert(first.draft == "unfinished reply");
        assert(!app.storage_.message(first.id, "older-mention-0"));
        assert(app.model.pendingSourceJump.revision > pending.revision);
        assert(app.model.pendingSourceJump.buffer.empty());
        assert(app.model.pendingSourceJump.message.empty());
        const auto clearedRequest = app.model.pendingSourceJump;
        app.action({"global-mentions", {}, {}, {}});
        app.action({"source-jump", first.id, "older-mention-0", {}});
        assert(first.messages.empty());
        assert(first.draft == "unfinished reply");
        assert(!app.storage_.message(first.id, "older-mention-0"));
        assert(app.model.activeBuffer == GlobalMentionsBufferId);
        assert(app.model.pendingSourceJump.revision == clearedRequest.revision);
        assert(app.model.pendingSourceJump.buffer.empty());
        assert(app.model.pendingSourceJump.message.empty());

        const auto secondId = second.id;
        app.renameBuffer(secondId, "#ROOM");
        app.refreshMentions();
        assert(second.id == secondId && second.target == "#ROOM" &&
               app.model.mentionSources.at(secondId).target == second.target);
        const auto &renamedFeed = app.model.buffers.at(GlobalMentionsBufferId).messages;
        assert(std::any_of(renamedFeed.begin(), renamedFeed.end(), [&](const auto &row) {
            return row.sourceBuffer == secondId && row.sourceId == "same-raw-id" &&
                   row.sourceTarget == second.target;
        }));

        // Equal server timestamps preserve durable insertion order, not lexical IDs,
        // even when the newer row is already loaded and the older row is not.
        Message tied;
        tied.id = "z-tie-first";
        tied.nick = "Speaker";
        tied.text = "Me: first at this instant";
        tied.timestamp = 1793836802;
        tied.time = "2026-11-05T00:00:02.123456789Z";
        app.storage_.append(second.id, tied);
        incoming.server = second.server;
        incoming.target = second.target;
        incoming.id = "a-tie-second";
        incoming.text = "Me: second at this instant";
        incoming.timestamp = tied.timestamp;
        incoming.time = tied.time;
        app.reduce(incoming);
        app.action({"source-jump", second.id, tied.id, {}});
        assert(app.model.pendingSourceJump.buffer == second.id &&
               app.model.pendingSourceJump.message == tied.id && second.messages.size() == 3 &&
               second.messages[1].id == tied.id && second.messages[2].id == incoming.id);

        // Provisional own sends belong only to queued-send recovery, never this cache.
        app.savingAllowed_ = false;
        Message unsavedOwn;
        unsavedOwn.id = "unsaved-own-pending";
        unsavedOwn.own = unsavedOwn.pending = true;
        unsavedOwn.text = "recover through queued sends";
        assert(!app.store(second, unsavedOwn, false) && app.unsavedSources_.empty());
        Message unsavedIncoming;
        unsavedIncoming.id = "unsaved-incoming";
        unsavedIncoming.nick = "Speaker";
        unsavedIncoming.text = "Me: retain incoming history";
        app.model.networks[second.server].nick.clear();
        assert(!app.store(second, unsavedIncoming, false));
        const auto &cached = app.unsavedSources_.at(second.id);
        assert(cached.server == second.server && cached.target == second.target &&
               cached.messages.size() == 1 && cached.messages.front().id == unsavedIncoming.id &&
               !cached.messages.front().own);
        app.insert(second, unsavedIncoming, false);
        app.refreshMentions();
        assert(!cached.messages.front().mention);
        app.model.networks[second.server].nick = "Me";
        app.refreshMentions();
        assert(cached.messages.front().mention && second.messages.back().mention);
        app.model.networks[second.server].nick = "DifferentNick";
        app.refreshMentions();
        const auto &retained = app.model.mentionSources.at(second.id).messages;
        assert(cached.messages.front().mention && second.messages.back().mention &&
               std::any_of(retained.begin(), retained.end(), [&](const auto &row) {
                   return row.id == unsavedIncoming.id && row.mention;
               }));
    }
    static void caseMappingFacts() {
        Application app;
        app.model = Model{};
        app.savingAllowed_ = false;
        app.model.settings.retentionDays = 0;
        app.model.settings.presence = PresenceMode::Smart;
        auto &network = app.model.networks["mapping-facts"];
        network.nick = "Me[";
        assert(network.caseMapping == "rfc1459");
        const string originalId = app.buffer("mapping-facts", "#[Room]").id;
        bool observedRename = false;
        app.model.bufferWillRename = [&](const string &from, const string &to) {
            observedRename = true;
            assert(network.caseMapping == "ascii" && app.fold("mapping-facts", "[") == "[");
            assert(from == originalId && to == "mapping-facts/#[room]" &&
                   app.model.buffers.contains(from) && !app.model.buffers.contains(to));
        };
        Event support;
        support.type = "network";
        support.server = "mapping-facts";
        support.tags = {{"kind", "isupport"}, {"CASEMAPPING", "ascii"}};
        app.reduce(support);
        assert(observedRename && network.caseMapping == "ascii" &&
               !app.model.buffers.contains(originalId));
        app.model.bufferWillRename = {};
        auto &asciiRoom = app.buffer(support.server, "#[Room]");
        Event join;
        join.type = "join";
        join.server = support.server;
        join.target = asciiRoom.target;
        join.nick = "Me{";
        join.id = "ascii-not-own";
        app.reduce(join);
        assert(!asciiRoom.joined && !asciiRoom.messages.back().own);
        join.nick = "Me[";
        join.id = "ascii-own";
        app.reduce(join);
        assert(asciiRoom.joined && asciiRoom.messages.back().own);

        support.tags["CASEMAPPING"] = "strict-rfc1459";
        app.reduce(support);
        assert(network.caseMapping == "strict-rfc1459" && app.fold(support.server, "[^") == "{^");
        network.nick = "Me^";
        auto &strictRoom = app.buffer(support.server, "#strict");
        join.target = strictRoom.target;
        join.nick = "Me~";
        join.id = "strict-not-own";
        app.reduce(join);
        assert(!strictRoom.joined && !strictRoom.messages.back().own);
        support.tags["CASEMAPPING"] = "rfc1459";
        app.reduce(support);
        assert(network.caseMapping == "rfc1459" && app.fold(support.server, "^") == "~");
        join.id = "rfc-own";
        app.reduce(join);
        assert(strictRoom.joined && strictRoom.messages.back().own);

        support.tags["CASEMAPPING"] = "ascii";
        app.reduce(support);
        support.tags = {{"kind", "isupport"}, {"-CASEMAPPING", ""}};
        app.reduce(support);
        assert(network.caseMapping == "rfc1459" && app.fold(support.server, "[^") == "{~");
        support.tags = {{"kind", "isupport"}, {"CASEMAPPING", "strict-rfc1459"}};
        app.reduce(support);
        support.tags = {{"kind", "isupport"}};
        app.reduce(support);
        assert(network.caseMapping == "rfc1459");
        support.tags["CASEMAPPING"] = "ascii";
        app.reduce(support);
        Event connecting;
        connecting.type = "state";
        connecting.server = support.server;
        connecting.text = "Connecting";
        app.reduce(connecting);
        assert(network.caseMapping == "rfc1459" && app.fold(support.server, "[^") == "{~");
    }
    static void truthfulActivity() {
        Application app;
        app.model = Model{};
        app.savingAllowed_ = false;
        app.messageCount_ = 0;
        app.model.now = 100;
        app.model.settings.retentionDays = 0;
        app.model.settings.presence = PresenceMode::Smart;
        app.model.networks["activity"].nick = "Me";
        auto &room = app.buffer("activity", "#activity");
        app.model.activeBuffer = room.id;
        app.model.settings.workspace["visible_buffers"] = room.id;
        Event event;
        event.server = room.server;
        event.target = room.target;
        event.timestamp = 1770000000;
        event.time = "2026-02-02T02:40:00Z";
        auto expectRow = [&](const Event &input, ActivityKind kind, const string &text) {
            const auto &row = room.messages.back();
            assert(row.id == input.id && row.nick == input.nick && row.time == input.time &&
                   row.timestamp == input.timestamp && row.text == text && row.presence &&
                   row.activity == kind && !row.action && row.history == input.history);
        };
        event.type = "join";
        event.nick = "Quiet";
        event.id = "quiet-join";
        app.reduce(event);
        expectRow(event, ActivityKind::Join, "Quiet joined #activity");
        assert(room.messages.size() == 1 && app.model.effects.empty());
        event.type = "part";
        event.id = "quiet-part";
        event.text = "leaving for lunch";
        app.reduce(event);
        expectRow(event, ActivityKind::Part, "Quiet left #activity — leaving for lunch");
        assert(room.messages.size() == 2 && app.model.effects.empty());
        event.type = "join";
        event.id = "quiet-rejoin";
        event.text.clear();
        app.reduce(event);
        event.type = "nick";
        event.id = "quiet-nick";
        event.text = "Renamed";
        app.reduce(event);
        expectRow(event, ActivityKind::Nick, "Quiet is now known as Renamed");
        assert(room.messages.back().activityTarget == "Renamed");
        event.type = "quit";
        event.id = "quiet-quit";
        event.nick = "Renamed";
        event.text = "connection closed";
        app.reduce(event);
        expectRow(event, ActivityKind::Quit, "Renamed quit: connection closed");
        assert(room.messages.size() == 5 && app.model.effects.empty());
        app.tick(111);
        assert(room.messages.size() == 5 && app.model.effects.empty());

        // /me remains an action, never an inferred presence/activity event.
        event.type = "action";
        event.id = "real-action";
        event.text = "joined the dance";
        app.reduce(event);
        assert(room.messages.back().id == event.id && room.messages.back().action &&
               !room.messages.back().presence &&
               room.messages.back().activity == ActivityKind::None);
        app.model.effects.clear();

        // Casemapped self JOINs retain typed history and membership, never live activity.
        app.model.networks["activity"].nick = "[Me]";
        app.model.networks["activity"].caseMapping = "rfc1459";
        room.joined = false;
        event.type = "join";
        event.nick = "{mE}";
        event.id = "own-live-join";
        event.tags = {{"account", "live-account"}};
        app.reduce(event);
        expectRow(event, ActivityKind::Join, "{mE} joined #activity");
        const auto self = std::find_if(room.nicks.begin(), room.nicks.end(),
                                       [&](const auto &nick) { return nick.name == event.nick; });
        assert(room.joined && self != room.nicks.end() && self->account == "live-account" &&
               room.messages.back().own && app.model.effects.empty());
        const auto members = room.nicks.size();
        room.joined = false;
        event.history = true;
        event.id = "own-history-join";
        event.tags["account"] = "historical-account";
        app.reduce(event);
        const auto historicalSelf =
            std::find_if(room.messages.begin(), room.messages.end(),
                         [&](const auto &row) { return row.id == event.id; });
        assert(historicalSelf != room.messages.end() && historicalSelf->own &&
               historicalSelf->history && historicalSelf->activity == ActivityKind::Join &&
               !room.joined && room.nicks.size() == members && self->account == "live-account" &&
               app.model.effects.empty());
        event.history = false;
        app.model.networks["activity"].nick = "Me";

        // Neither ordering lets a self JOIN consume the other member's aggregate effect.
        for (const bool selfFirst : {true, false}) {
            const auto before = room.messages.size();
            event.tags = {{"batch", selfFirst ? "self-first" : "self-last"},
                          {"batch-type", "netjoin"}};
            for (int i = 0; i < 2; ++i) {
                const bool own = (i == 0) == selfFirst;
                event.nick = own ? "mE" : "Other";
                event.id = (selfFirst ? "self-first-" : "self-last-") + std::to_string(i);
                app.reduce(event);
                expectRow(event, ActivityKind::Join, event.nick + " joined #activity");
                assert(room.messages.back().own == own);
                if (own && i == 0)
                    assert(app.model.effects.empty());
            }
            assert(room.messages.size() == before + 2 && app.model.effects.size() == 1 &&
                   app.model.effects.front().type == "join" &&
                   app.model.effects.front().nick == "Other" && !app.model.effects.front().own);
            app.tick(app.model.now + 11);
            assert(app.model.effects.empty());
        }

        // Explicit batches retain every original member, never a synthetic summary row.
        for (const bool joins : {true, false}) {
            const auto before = room.messages.size();
            for (int i = 0; i < 3; ++i) {
                event.type = joins ? "join" : "quit";
                event.nick = "Split" + std::to_string(i);
                event.id = (joins ? "netjoin-" : "netsplit-") + std::to_string(i);
                event.text = joins ? "" : "irc.one.example irc.two.example";
                event.timestamp = 1770000010 + i;
                event.time = "2026-02-02T02:40:1" + std::to_string(i) + "Z";
                event.tags = {{"batch", joins ? "join-batch" : "split-batch"},
                              {"batch-type", joins ? "netjoin" : "netsplit"}};
                app.reduce(event);
                assert(room.messages.size() == before + i + 1);
                expectRow(event, joins ? ActivityKind::Join : ActivityKind::Quit,
                          event.nick + (joins ? " joined #activity"
                                              : " quit: irc.one.example irc.two.example"));
            }
            assert(app.model.effects.size() == 1 &&
                   app.model.effects.front().type == (joins ? "join" : "netsplit"));
            app.tick(app.model.now + 11);
            assert(room.messages.size() == before + 3 && app.model.effects.empty());
            app.model.effects.clear();
        }

        // Hidden still suppresses strangers, including explicit batches.
        app.model.settings.presence = PresenceMode::Hidden;
        const auto beforeHidden = room.messages.size();
        event.tags.clear();
        event.nick = "Hidden";
        for (const auto *type : {"join", "nick", "part", "join", "quit"}) {
            event.type = type;
            event.id = "hidden-" + string(type);
            event.text = event.type == "nick" ? "Hidden" : "hidden reason";
            app.reduce(event);
            assert(room.messages.size() == beforeHidden && app.model.effects.empty());
        }
        for (const bool joins : {true, false}) {
            event.type = joins ? "join" : "quit";
            event.id = joins ? "hidden-netjoin" : "hidden-netsplit";
            event.tags = {{"batch", joins ? "hidden-join" : "hidden-split"},
                          {"batch-type", joins ? "netjoin" : "netsplit"}};
            app.reduce(event);
            assert(room.messages.size() == beforeHidden && app.model.effects.empty());
        }
        event.tags.clear();
        event.type = "join";
        event.nick = "Me";
        event.id = "hidden-own";
        app.reduce(event);
        expectRow(event, ActivityKind::Join, "Me joined #activity");
        assert(room.messages.back().own && app.model.effects.empty());
        event.type = "part";
        event.id = "hidden-own-part";
        event.own = true;
        app.reduce(event);
        expectRow(event, ActivityKind::Part, "Me left #activity — hidden reason");
        assert(room.messages.back().own && app.model.effects.size() == 1 &&
               app.model.effects.front().type == "part");
        event.type = "join";
        event.own = false;
        event.nick = "Forced";
        event.id = "hidden-forced";
        event.tags["force-presence"] = "1";
        app.reduce(event);
        expectRow(event, ActivityKind::Join, "Forced joined #activity");
        assert(room.messages.size() == beforeHidden + 3 && app.model.effects.size() == 2 &&
               app.model.effects.back().nick == "Forced");

        app.model.settings.presence = PresenceMode::Smart;
        app.model.effects.clear();
        event.tags.clear();
        event.type = "message";
        event.nick = "Speaker";
        event.id = "speaker-first";
        event.text = "hello";
        app.reduce(event);
        app.model.effects.clear();
        event.type = "join";
        event.id = "speaker-join";
        const auto beforeSpeaker = room.messages.size();
        app.reduce(event);
        expectRow(event, ActivityKind::Join, "Speaker joined #activity");
        assert(app.model.effects.size() == 1);
        event.type = "message";
        event.id = "speaker-again";
        event.text = "still here";
        app.reduce(event);
        assert(room.messages.size() == beforeSpeaker + 2 && app.model.effects.size() == 2 &&
               std::count_if(room.messages.begin(), room.messages.end(),
                             [](const auto &row) { return row.id == "speaker-join"; }) == 1);

        app.model.effects.clear();
        event.history = true;
        event.nick = "Historical";
        const auto beforeHistory = room.messages.size();
        for (const auto &[type, kind] :
             {std::pair{"join", ActivityKind::Join}, std::pair{"part", ActivityKind::Part},
              std::pair{"nick", ActivityKind::Nick}, std::pair{"quit", ActivityKind::Quit}}) {
            event.type = type;
            event.id = "history-activity-" + string(type);
            event.text = "original history reason";
            app.reduce(event);
            const auto found = std::find_if(room.messages.begin(), room.messages.end(),
                                            [&](const auto &row) { return row.id == event.id; });
            assert(found != room.messages.end() && found->activity == kind && found->presence &&
                   found->history && found->nick == event.nick && found->time == event.time &&
                   found->timestamp == event.timestamp);
        }
        assert(room.messages.size() == beforeHistory + 4 && app.model.effects.empty());
    }
    static void globalRead() {
        RenameDrainPeer peer;
        Application app;
        app.model = Model{};
        app.model.settings.retentionDays = 0;
        Identity identity;
        identity.nick = "Me";
        ServerConfig server;
        server.name = "global-read";
        server.host = "127.0.0.1";
        server.port = peer.port;
        server.tls = false;
        app.model.identities = {identity};
        app.model.servers = {server};
        app.clients_[server.name] = std::make_unique<NetworkClient>(server, identity);
        auto &client = *app.clients_.at(server.name);
        client.start();
        peer.accept();
        peer.expect("CAP", {"LS", "302"});
        peer.send(":server CAP * LS :draft/read-marker");
        peer.expect("NICK", {identity.nick});
        peer.expect("USER", {identity.username, "0", "*", identity.realname});
        peer.expect("CAP", {"REQ", "draft/read-marker"});
        peer.send(":server CAP Me ACK :draft/read-marker");
        peer.expect("CAP", {"END"});
        peer.send(":server 001 Me :Welcome");
        peer.send(":server 376 Me :End of MOTD");
        const auto deadline = RenameDrainPeer::Clock::now() + std::chrono::seconds(3);
        while (app.model.networks[server.name].state != "Ready") {
            for (const auto &event : client.poll())
                app.reduce(event);
            RenameDrainPeer::require(RenameDrainPeer::Clock::now() < deadline,
                                     "Global-read registration deadline expired");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        auto &room = app.buffer(server.name, "#room");
        auto &offline = app.buffer("global-read-offline", "Peer", "query");
        auto &search = app.buffer(server.name, "*search*", "search");
        Message row;
        row.id = "global-read-message";
        row.time = "2026-11-05T00:00:00.900Z";
        row.text = "keep this history";
        room.messages.push_back(row);
        room.unread = 3;
        room.draft = "keep this draft";
        row.id = "local:global-read";
        row.time.clear();
        offline.messages.push_back(row);
        offline.unread = 2;
        row.pending = true;
        room.messages.push_back(row);
        row.pending = false;
        row.presence = true;
        room.messages.push_back(row);
        search.messages.push_back(row);
        search.unread = 9;
        app.model.activeBuffer = room.id;
        app.action({"read-all", {}, {}, {}});
        peer.expect("MARKREAD", {"#room", "timestamp=2026-11-05T00:00:00.900Z"});
        assert(room.unread == 0 && room.readMarker == "2026-11-05T00:00:00.900Z");
        assert(offline.unread == 0 && offline.readMarker == "local:local:global-read");
        assert(search.unread == 9 && search.readMarker.empty());
        assert(room.messages.size() == 3 && room.draft == "keep this draft" &&
               app.model.activeBuffer == room.id);
        Model reloaded;
        app.storage_.load(reloaded);
        assert(reloaded.buffers.at(room.id).readMarker == room.readMarker &&
               reloaded.buffers.at(offline.id).readMarker == offline.readMarker);

        // A real database write failure leaves the failed buffer retryable and
        // cannot overwrite failure feedback with a blanket success message.
        sqlite3 *db = nullptr;
        const auto database =
            std::filesystem::path(std::getenv("XDG_DATA_HOME")) / "blastirc/state.sqlite3";
        assert(sqlite3_open(database.c_str(), &db) == SQLITE_OK);
        assert(sqlite3_exec(db,
                            "CREATE TRIGGER reject_global_read BEFORE UPDATE OF read_marker "
                            "ON buffers WHEN NEW.id='global-read/#room' BEGIN "
                            "SELECT RAISE(FAIL, 'read marker rejected'); END;",
                            nullptr, nullptr, nullptr) == SQLITE_OK);
        room.messages.front().time = "2026-11-05T00:00:01.900Z";
        room.unread = 1;
        offline.unread = 1;
        app.action({"read-all", {}, {}, {}});
        assert(room.unread == 1 && room.readMarker == "2026-11-05T00:00:00.900Z");
        assert(offline.unread == 0 &&
               app.model.status.find("read marker rejected") != string::npos);
        assert(sqlite3_exec(db, "DROP TRIGGER reject_global_read", nullptr, nullptr, nullptr) ==
               SQLITE_OK);
        sqlite3_close(db);
        app.action({"read-all", {}, {}, {}});
        peer.expect("MARKREAD", {"#room", "timestamp=2026-11-05T00:00:01.900Z"});
        assert(room.unread == 0 && room.readMarker == room.messages.front().time);
        // Disconnected clients remain retained for reconnect; they reject
        // enqueues but must not prevent durable local read advancement.
        app.stopClient(server.name);
        assert(app.client(server.name) && app.model.networks.at(server.name).state != "Ready");
        room.messages.front().time = "2026-11-05T00:00:02.900Z";
        room.unread = 1;
        app.action({"read-all", {}, {}, {}});
        assert(room.unread == 0 && room.readMarker == room.messages.front().time &&
               app.model.status == "All buffers marked as read");
        Model disconnectedReload;
        app.storage_.load(disconnectedReload);
        assert(disconnectedReload.buffers.at(room.id).readMarker == room.readMarker);
    }
    static void historyReadyWithoutIsupport() {
        for (const int numeric : {376, 422}) {
            RenameDrainPeer peer;
            Application app;
            app.model = Model{};
            app.savingAllowed_ = false;
            Identity identity;
            identity.nick = "Me";
            ServerConfig config;
            config.name = "history-defaults-" + std::to_string(numeric);
            config.host = "127.0.0.1";
            config.port = peer.port;
            config.tls = false;
            app.model.servers = {config};
            app.clients_[config.name] = std::make_unique<NetworkClient>(config, identity);
            auto &client = *app.clients_.at(config.name);
            client.start();
            peer.accept();
            peer.expect("CAP", {"LS", "302"});
            peer.send(":server CAP * LS :batch draft/chathistory");
            peer.expect("NICK", {identity.nick});
            peer.expect("USER", {identity.username, "0", "*", identity.realname});
            peer.expect("CAP", {"REQ", "batch draft/chathistory"});
            peer.send(":server CAP Me ACK :batch draft/chathistory");
            peer.expect("CAP", {"END"});
            peer.send(":server 001 Me :Welcome");
            peer.fence();
            for (const auto &event : client.poll())
                app.reduce(event);
            app.scheduleHistory();
            assert(app.historyPages_.empty()); // CAP/001 is not settled ISUPPORT.
            // Real MOTD completion, deliberately no 005 or invented ISUPPORT.
            peer.send(":server " + std::to_string(numeric) + " Me :MOTD settled");
            bool fabricatedChannelTypes = false, settled = false;
            const auto wait = [&](const auto &predicate) {
                const auto deadline = RenameDrainPeer::Clock::now() + std::chrono::seconds(5);
                do {
                    for (const auto &event : client.poll()) {
                        if (event.type == "network" && event.tags.contains("kind")) {
                            fabricatedChannelTypes |= event.tags.at("kind") == "isupport" &&
                                                      event.tags.contains("CHANTYPES");
                            settled |= event.tags.at("kind") == "isupport-settled";
                        }
                        app.reduce(event);
                    }
                    app.scheduleHistory();
                    if (predicate())
                        return;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                } while (RenameDrainPeer::Clock::now() < deadline);
                throw std::runtime_error("History readiness without ISUPPORT timed out");
            };
            wait([&] { return !app.historyPages_.empty(); });
            assert(settled && !fabricatedChannelTypes);
            const auto request = peer.read();
            assert(request.command == "CHATHISTORY" && request.params.size() == 4 &&
                   request.params[0] == "TARGETS");
            peer.send("@draft/chathistory-end :server BATCH +defaults draft/chathistory-targets");
            peer.send("@batch=defaults :server CHATHISTORY TARGETS #unknown "
                      "2026-11-01T00:00:00.000Z");
            peer.send(":server BATCH -defaults");
            wait([&] { return app.historySessions_.at(config.name).planned; });
            assert(
                std::none_of(app.model.buffers.begin(), app.model.buffers.end(),
                             [](const auto &entry) { return entry.second.target == "#unknown"; }));
        }
    }
    static void zncReconnectPlaybackCursor() {
        RenameDrainPeer peer;
        Application app;
        app.model = Model{};
        app.savingAllowed_ = false;
        Identity identity;
        identity.nick = "Me";
        ServerConfig config;
        config.name = "znc-reconnect-cursor";
        config.host = "127.0.0.1";
        config.port = peer.port;
        config.tls = false;
        config.bouncer = "znc";
        app.model.servers = {config};
        auto &room = app.buffer(config.name, "Peer", "query");
        Message anchor;
        anchor.id = "opaque-playback-anchor";
        anchor.time = "2026-11-01T00:00:00.125Z";
        room.messages.push_back(anchor);
        room.historyState.newest = {"protocol-anchor", "2026-11-01T00:00:00.124Z"};
        Message ignored = anchor;
        ignored.time = "2027-11-01T00:00:00.000Z";
        ignored.id = "pending-future";
        ignored.pending = true;
        room.messages.push_back(ignored);
        ignored.pending = false;
        ignored.id = "local:future";
        room.messages.push_back(ignored);
        ignored.id = "local-future";
        room.messages.push_back(ignored);
        ignored.id = "invalid-time";
        ignored.time = "not-a-timestamp";
        room.messages.push_back(ignored);
        ignored.id = "older-server-row";
        ignored.time = "2026-10-31T00:00:00.000Z";
        room.messages.push_back(ignored); // Never use vector order as a time highwater.
        app.clients_[config.name] = std::make_unique<NetworkClient>(config, identity);
        auto &client = *app.clients_.at(config.name);
        client.start();
        peer.accept();
        peer.expect("CAP", {"LS", "302"});
        peer.send(":server CAP * LS :batch znc.in/playback");
        peer.expect("NICK", {identity.nick});
        peer.expect("USER", {identity.username, "0", "*", identity.realname});
        peer.expect("CAP", {"REQ", "batch znc.in/playback"});
        peer.send(":server CAP Me ACK :batch znc.in/playback");
        peer.expect("CAP", {"END"});
        peer.send(":server 001 Me :Welcome");
        peer.send(":server 376 Me :End of MOTD");
        const auto deadline = RenameDrainPeer::Clock::now() + std::chrono::seconds(5);
        do {
            for (const auto &event : client.poll())
                app.reduce(event);
            app.requestHistory(room, "", true);
            app.scheduleHistory();
            if (!app.historyPages_.empty())
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (RenameDrainPeer::Clock::now() < deadline);
        assert(!app.historyPages_.empty() && app.historySessions_.empty());
        peer.expect("ZNC", {"*playback", "PLAY Peer 1793491200.125"});
        assert(app.historyPages_.begin()->second.request.direction == HistoryDirection::After);
        assert(app.historyPages_.begin()->second.request.first.id.empty() &&
               app.historyPages_.begin()->second.request.first.time == anchor.time);
        app.cancelHistory(room);
        peer.send(":server BATCH +retired-playback znc.in/playback Peer");
        peer.send(":server BATCH -retired-playback");
        peer.fence(); // Cancellation does not steal an existing unlabeled holder.
        auto &empty = app.buffer(config.name, "NoCache", "query");
        ignored.id = "local:no-trusted-cache";
        ignored.time = "2027-11-01T00:00:00.000Z";
        empty.messages.push_back(ignored);
        ignored.id = "pending-no-trusted-cache";
        ignored.pending = true;
        empty.messages.push_back(ignored);
        app.requestHistory(empty, "", true);
        app.scheduleHistory();
        peer.expect("ZNC", {"*playback", "PLAY NoCache 0"});
        assert(app.historyPages_.size() == 1 &&
               app.historyPages_.begin()->second.request.direction == HistoryDirection::Latest &&
               app.historyPages_.begin()->second.request.first.time.empty() &&
               app.historyPages_.begin()->second.request.first.id.empty());
    }
    static void historyRenameContinuity() {
        Application app;
        app.model = Model{};
        app.model.settings.retentionDays = 0;
        const string server = "history-rename";
        auto &source = app.buffer(server, "#old");
        auto &destination = app.buffer(server, "#new");
        const string oldId = source.id, newId = destination.id;
        const auto island = [&](Buffer &b, const string &prefix, const string &day) {
            for (int i = 0; i < 2; ++i) {
                Message message;
                message.id = prefix + std::to_string(i);
                message.time = "2026-11-" + day + "T0" + std::to_string(i) + ":00:00.000Z";
                message.text = "retained island endpoint";
                app.storage_.append(b.id, message);
                b.messages.push_back(*app.storage_.message(b.id, message.id));
                ++app.messageCount_;
            }
            b.historyState.oldest = {b.messages.front().id, b.messages.front().time};
            b.historyState.newest = {b.messages.back().id, b.messages.back().time};
            HistoryGap gap;
            gap.older = b.historyState.oldest;
            gap.newer = b.historyState.newest;
            b.historyState.gaps.push_back(gap);
            app.storage_.saveHistoryState(b);
        };
        island(source, "source-", "01");
        island(destination, "destination-", "03");
        const auto sourceGap = source.historyState.gaps.front();
        const auto destinationGap = destination.historyState.gaps.front();
        auto &session = app.historySessions_[server];
        session.visible = {oldId};
        session.background = {oldId};
        session.seeds = {oldId};
        session.refused.insert(oldId);
        Application::HistoryPage retry;
        retry.server = server;
        retry.buffer = oldId;
        retry.request.target = "#old";
        session.retries.push_back(retry);
        app.renameBuffer(oldId, "#new");
        assert(!app.model.buffers.contains(oldId));
        assert(session.visible.front() == newId && session.background.front() == newId &&
               session.seeds.front() == newId && session.refused.contains(newId) &&
               !session.refused.contains(oldId));
        assert(session.retries.front().buffer == newId &&
               session.retries.front().request.target == "#new");
        auto &merged = app.model.buffers.at(newId);
        assert(merged.historyState.oldest.id == "source-0" &&
               merged.historyState.newest.id == "destination-1" &&
               merged.historyState.gaps.size() == 3);
        assert(std::any_of(
            merged.historyState.gaps.begin(), merged.historyState.gaps.end(), [](const auto &gap) {
                return gap.older.id == "source-1" && gap.older.time == "2026-11-01T01:00:00.000Z" &&
                       gap.newer.id == "destination-0" &&
                       gap.newer.time == "2026-11-03T00:00:00.000Z";
            }));
        for (const auto &original : {sourceGap, destinationGap})
            assert(std::any_of(merged.historyState.gaps.begin(), merged.historyState.gaps.end(),
                               [&](const auto &gap) {
                                   return gap.id == original.id && gap.older == original.older &&
                                          gap.newer == original.newer;
                               }));
        const auto expectedGaps = merged.historyState.gaps;
        Buffer canonical = merged;
        canonical.historyState = {};
        app.storage_.loadHistoryState(canonical);
        assert(canonical.historyState.oldest == merged.historyState.oldest &&
               canonical.historyState.newest == merged.historyState.newest &&
               canonical.historyState.gaps.size() == merged.historyState.gaps.size());
        // A later overlapping page must not overwrite the destination's unioned gaps
        // with the old source's pre-rename in-memory metadata.
        Application::HistoryPage page;
        page.server = server;
        page.buffer = newId;
        page.request.target = "#new";
        page.request.direction = HistoryDirection::Latest;
        Event overlap;
        overlap.type = "message";
        overlap.server = server;
        overlap.target = "#new";
        overlap.id = "destination-1";
        overlap.time = "2026-11-03T01:00:00.000Z";
        overlap.text = "retained island endpoint";
        overlap.history = true;
        page.events.push_back(overlap);
        Event end;
        end.tags = {{"direction", "latest"},       {"count", "1"},
                    {"oldest-id", overlap.id},     {"newest-id", overlap.id},
                    {"oldest-time", overlap.time}, {"newest-time", overlap.time}};
        app.applyHistoryPage(page, end);
        app.storage_.loadHistoryState(canonical);
        assert(merged.historyState.gaps.size() == 3 && canonical.historyState.gaps.size() == 3);
        for (const auto &gap : expectedGaps) {
            assert(std::any_of(merged.historyState.gaps.begin(), merged.historyState.gaps.end(),
                               [&](const auto &saved) {
                                   return saved.id == gap.id && saved.older == gap.older &&
                                          saved.newer == gap.newer;
                               }));
            assert(std::any_of(canonical.historyState.gaps.begin(),
                               canonical.historyState.gaps.end(), [&](const auto &saved) {
                                   return saved.id == gap.id && saved.older == gap.older &&
                                          saved.newer == gap.newer;
                               }));
        }
    }
    static void historyMutationTargetsAreNotPageCoverage() {
        for (const string kind : {"reaction", "redact"})
            for (const bool context : {false, true}) {
                Application app;
                app.model = Model{};
                app.model.settings.retentionDays = 0;
                app.storage_.prune(app.model.settings);
                auto &room = app.buffer("mutation-coverage-" + kind + (context ? "-context" : ""),
                                        "Peer", "query");
                Message prior;
                prior.id = "protocol-old";
                prior.nick = "Other";
                prior.text = "original retained boundary";
                prior.time = "2026-11-01T00:00:00.000Z";
                prior.timestamp = 1793491200;
                prior.history = true;
                app.storage_.append(room.id, prior);
                room.messages.push_back(*app.storage_.message(room.id, prior.id));
                app.messageCount_ = 1;
                room.historyState.oldest = room.historyState.newest = {prior.id, prior.time};
                app.storage_.saveHistoryState(room);
                Application::HistoryPage page;
                page.server = room.server;
                page.buffer = room.id;
                page.request.target = room.target;
                page.request.direction = HistoryDirection::Latest;
                Event mutation;
                mutation.type = kind;
                mutation.server = room.server;
                mutation.target = room.target;
                mutation.nick = "Reactor";
                mutation.id = prior.id; // Mutation target, not this wire event's identity.
                mutation.tags["msgid"] = kind + "-wire-event";
                mutation.time = "2026-11-02T00:00:00.000Z";
                mutation.history = true;
                mutation.text = kind == "reaction" ? "+1" : "removed";
                if (context)
                    mutation.tags["draft/chathistory-context"] = "1";
                page.events.push_back(mutation);
                Event newest;
                newest.type = "message";
                newest.server = room.server;
                newest.target = room.target;
                newest.nick = "Other";
                newest.id = "new-primary";
                newest.tags["msgid"] = newest.id;
                newest.time = "2026-11-02T01:00:00.000Z";
                newest.text = "new primary page";
                newest.history = true;
                page.events.push_back(newest);
                const string expectedEdge = context ? newest.id : mutation.tags.at("msgid");
                Event end;
                end.tags = {{"direction", "latest"},
                            {"count", context ? "1" : "2"},
                            {"oldest-id", expectedEdge},
                            {"oldest-time", context ? newest.time : mutation.time},
                            {"newest-id", newest.id},
                            {"newest-time", newest.time}};
                app.applyHistoryPage(page, end);
                assert(room.historyState.gaps.size() == 1);
                const auto gap = room.historyState.gaps.front();
                assert(gap.id > 0 && gap.older.id == prior.id && gap.older.time == prior.time &&
                       gap.newer.id == expectedEdge);
                const auto assertMutation = [&] {
                    const auto durable = app.storage_.message(room.id, prior.id);
                    const auto *loaded = sourceMessage(app.model, room.id, prior.id);
                    assert(durable);
                    if (kind == "redact") {
                        assert(durable->deleted && durable->text.empty());
                        const auto retained =
                            std::find_if(room.messages.begin(), room.messages.end(),
                                         [&](const auto &row) { return row.id == prior.id; });
                        assert(retained != room.messages.end() && retained->deleted &&
                               retained->text.empty() && !loaded);
                    } else {
                        assert(loaded && loaded->reactions.at("+1") == vector<string>{"Reactor"} &&
                               durable->reactions.at("+1") == vector<string>{"Reactor"});
                    }
                };
                assertMutation();
                Buffer restored = room;
                restored.historyState = {};
                app.storage_.loadHistoryState(restored);
                assert(restored.historyState.gaps.size() == 1 &&
                       restored.historyState.gaps.front().id == gap.id);
                // Actual replay of the old boundary row, unlike a mutation that
                // merely targets it, can close the selected directional gap.
                Application::HistoryPage fill;
                fill.server = room.server;
                fill.buffer = room.id;
                fill.gapId = gap.id;
                fill.request.target = room.target;
                fill.request.direction = HistoryDirection::Before;
                fill.request.first = gap.newer;
                Event replay;
                replay.type = "message";
                replay.server = room.server;
                replay.target = room.target;
                replay.id = prior.id;
                replay.tags["msgid"] = prior.id;
                replay.nick = prior.nick;
                replay.text = prior.text;
                replay.time = prior.time;
                replay.timestamp = prior.timestamp;
                replay.history = true;
                fill.events.push_back(replay);
                end.tags = {{"direction", "before"},     {"count", "1"},
                            {"oldest-id", prior.id},     {"newest-id", prior.id},
                            {"oldest-time", prior.time}, {"newest-time", prior.time}};
                app.applyHistoryPage(fill, end);
                assert(room.historyState.gaps.empty());
                app.storage_.loadHistoryState(restored);
                assert(restored.historyState.gaps.empty());
                assertMutation(); // Replaying history never undoes the mutation.
            }
    }
    static void latestGapCanonicalPrimaryOverlap() {
        for (const bool contextOnly : {false, true}) {
            Application app;
            app.model = Model{};
            app.model.settings.retentionDays = 0;
            app.storage_.prune(app.model.settings);
            auto &room = app.buffer(contextOnly ? "latest-context-not-proof"
                                                : "latest-timestamp-canonical-proof",
                                    "Peer", "query");
            Message prior;
            prior.id = contextOnly ? "protocol-context" : "local:timestamp-protocol";
            prior.nick = "Other";
            prior.text = "original timestamp-only message";
            prior.time = "2026-11-01T00:00:00.000Z";
            prior.timestamp = 1793491200;
            prior.history = true;
            app.storage_.append(room.id, prior);
            room.messages.push_back(*app.storage_.message(room.id, prior.id));
            app.messageCount_ = 1;
            room.historyState.oldest =
                room.historyState.newest = {contextOnly ? prior.id : string{}, prior.time};
            app.storage_.saveHistoryState(room);
            Application::HistoryPage page;
            page.server = room.server;
            page.buffer = room.id;
            page.request.target = room.target;
            page.request.direction = HistoryDirection::Latest;
            Event replay;
            replay.type = "message";
            replay.server = room.server;
            replay.target = room.target;
            replay.nick = prior.nick;
            replay.text = prior.text;
            replay.time = prior.time;
            replay.timestamp = prior.timestamp;
            replay.id = contextOnly ? prior.id : string{}; // No wire msgid on primary replay.
            replay.history = true;
            if (contextOnly)
                replay.tags["draft/chathistory-context"] = "1";
            page.events.push_back(replay);
            Event primary = replay;
            primary.id = "new-primary";
            primary.text = "new primary page";
            primary.time = "2026-11-02T00:00:00.000Z";
            primary.timestamp = 1793577600;
            primary.tags.clear();
            page.events.push_back(primary);
            Event end;
            end.tags = {{"direction", "latest"},
                        {"count", contextOnly ? "1" : "2"},
                        {"oldest-id", contextOnly ? primary.id : string{}},
                        {"oldest-time", contextOnly ? primary.time : prior.time},
                        {"newest-id", primary.id},
                        {"newest-time", primary.time}};
            app.applyHistoryPage(page, end);
            assert(room.messages.size() == 2 && app.storage_.history(room.id, -1).size() == 2);
            assert(std::count_if(room.messages.begin(), room.messages.end(),
                                 [&](const auto &row) { return row.id == prior.id; }) == 1);
            Buffer restored = room;
            restored.historyState = {};
            app.storage_.loadHistoryState(restored);
            assert(room.historyState.gaps.size() == (contextOnly ? 1u : 0u) &&
                   restored.historyState.gaps.size() == room.historyState.gaps.size());
            if (contextOnly) {
                const auto &gap = room.historyState.gaps.front();
                assert(gap.older.id == prior.id && gap.older.time == prior.time &&
                       gap.newer.id == primary.id &&
                       restored.historyState.gaps.front().id == gap.id);
            }
        }
    }
    static void latestGapUsesProtocolBoundary() {
        for (const bool includesProtocolBoundary : {false, true}) {
            Application app;
            app.model = Model{};
            app.model.settings.retentionDays = 0;
            app.storage_.prune(app.model.settings);
            auto &room = app.buffer(includesProtocolBoundary ? "latest-protocol-overlap"
                                                             : "latest-protocol-gap",
                                    "#room");
            const auto retain = [&](const string &id, const string &time, bool presence) {
                Message row;
                row.id = id;
                row.time = time;
                row.text = presence ? "own JOIN" : "cached chat";
                row.presence = row.own = presence;
                app.storage_.append(room.id, row);
                room.messages.push_back(*app.storage_.message(room.id, id));
                ++app.messageCount_;
            };
            retain("protocol-join", "2026-11-01T00:00:00.000Z", true);
            room.historyState.oldest =
                room.historyState.newest = {"protocol-join", "2026-11-01T00:00:00.000Z"};
            app.storage_.saveHistoryState(room);
            retain("later-cached-chat", "2026-11-01T01:00:00.000Z", false);
            retain("new-live-join", "2026-11-02T04:00:00.000Z", true);
            Application::HistoryPage page;
            page.server = room.server;
            page.buffer = room.id;
            page.request.target = room.target;
            page.request.direction = HistoryDirection::Latest;
            const auto primary = [&](const string &id, const string &time, bool join) {
                Event row;
                row.type = join ? "join" : "message";
                row.server = room.server;
                row.target = room.target;
                row.nick = join ? "Me" : "Other";
                row.id = id;
                row.time = time;
                row.text = join ? "" : "offline chat";
                row.history = true;
                row.own = join;
                page.events.push_back(std::move(row));
            };
            if (includesProtocolBoundary)
                primary("protocol-join", "2026-11-01T00:00:00.000Z", true);
            primary("offline-chat", "2026-11-02T02:00:00.000Z", false);
            primary("new-live-join", "2026-11-02T04:00:00.000Z", true);
            Event end;
            end.tags = {{"direction", "latest"},
                        {"count", includesProtocolBoundary ? "3" : "2"},
                        {"oldest-id", page.events.front().id},
                        {"oldest-time", page.events.front().time},
                        {"newest-id", "new-live-join"},
                        {"newest-time", "2026-11-02T04:00:00.000Z"}};
            app.applyHistoryPage(page, end);
            Buffer restored = room;
            restored.historyState = {};
            app.storage_.loadHistoryState(restored);
            assert(room.historyState.gaps.size() == (includesProtocolBoundary ? 0u : 1u) &&
                   restored.historyState.gaps.size() == room.historyState.gaps.size());
            if (!includesProtocolBoundary) {
                const auto &gap = room.historyState.gaps.front();
                assert(gap.id > 0 && gap.older.id == "protocol-join" &&
                       gap.older.time == "2026-11-01T00:00:00.000Z" &&
                       gap.newer.id == "offline-chat" &&
                       restored.historyState.gaps.front().id == gap.id);
            }
        }
    }
    static void prunedHistoryCannotReproveEndpoints() {
        Application app;
        app.model = Model{};
        app.model.settings.retentionDays = 0;
        app.storage_.prune(app.model.settings);
        auto &room = app.buffer("pruned-history-proof", "Peer", "query");
        Message old;
        old.id = "pruned-but-visible";
        old.time = "1970-01-02T00:00:00.000Z";
        old.timestamp = 86400;
        old.text = "retained in RAM after pruning";
        app.storage_.append(room.id, old);
        room.messages.push_back(*app.storage_.message(room.id, old.id));
        assert(room.messages.front().storageSequence > 0);
        const auto prunedSequence = room.messages.front().storageSequence;
        app.messageCount_ = 1;
        room.historyState.oldest = room.historyState.newest = {old.id, old.time};
        room.historyState.beginning = true;
        app.storage_.saveHistoryState(room);
        auto retention = app.model.settings;
        retention.retentionDays = 1;
        app.storage_.prune(retention);
        assert(!app.storage_.message(room.id, old.id));
        app.storage_.loadHistoryState(room);
        assert(room.messages.size() == 1 && room.historyState.oldest.id.empty() &&
               room.historyState.newest.id.empty() && !room.historyState.beginning);
        Application::HistoryPage page;
        page.server = room.server;
        page.buffer = room.id;
        page.request.target = room.target;
        page.request.direction = HistoryDirection::Latest;
        Event end;
        end.tags = {{"direction", "latest"}, {"count", "0"}, {"limit", "50"}};
        app.applyHistoryPage(page, end);
        Buffer restored = room;
        restored.historyState = {};
        app.storage_.loadHistoryState(restored);
        for (const auto *state : {&room.historyState, &restored.historyState})
            assert(state->oldest.id.empty() && state->oldest.time.empty() &&
                   state->newest.id.empty() && state->newest.time.empty() && state->gaps.empty() &&
                   !state->beginning);
        app.storage_.prune(app.model.settings);
        assert(!app.storage_.message(room.id, old.id)); // Empty pages cannot resurrect RAM.
        Event returned;
        returned.type = "message";
        returned.server = room.server;
        returned.target = room.target;
        returned.id = old.id;
        returned.nick = "Other";
        returned.text = old.text;
        returned.time = old.time;
        returned.timestamp = old.timestamp;
        returned.history = true;
        completeOwnedHistory(app, room, returned, "pruned-row-owned-around");
        const auto rematerialized = app.storage_.message(room.id, old.id);
        assert(rematerialized && room.messages.size() == 1);
        const auto &loaded = room.messages.front();
        assert(loaded.id == old.id && rematerialized->id == old.id && loaded.time == old.time &&
               rematerialized->time == old.time && loaded.timestamp == old.timestamp &&
               rematerialized->timestamp == old.timestamp && loaded.text == old.text &&
               rematerialized->text == old.text);
        assert(loaded.storageSequence > 0 && loaded.storageSequence != prunedSequence &&
               loaded.historyOrder >= 0 &&
               loaded.storageSequence == rematerialized->storageSequence &&
               loaded.historyOrder == rematerialized->historyOrder);
    }
    static void sparseAroundRollbackPreservesLoadedWindow() {
        Application app;
        app.model = Model{};
        app.model.settings.retentionDays = 0;
        app.storage_.prune(app.model.settings);
        auto &room = app.buffer("sparse-around-rollback", "Peer", "query");
        for (int i = 0; i < 12; ++i) {
            Message row;
            row.id = "sparse-" + std::to_string(i);
            row.text = "original text " + std::to_string(i);
            row.time =
                "2026-11-01T" + string(i < 10 ? "0" : "") + std::to_string(i) + ":00:00.000Z";
            app.storage_.append(room.id, row);
        }
        const vector<string> originalIds{"sparse-1", "sparse-5", "sparse-9"};
        for (const auto &id : originalIds) {
            room.sourceMessageIndex[id] = room.messages.size();
            room.messages.push_back(*app.storage_.message(room.id, id));
        }
        app.messageCount_ = room.messages.size();
        app.model.activeBuffer = room.id;
        app.model.pendingSourceJump = {77, room.id, "sparse-5"};
        const auto anchor = app.model.pendingSourceJump;
        sqlite3 *fault = nullptr;
        const auto database =
            std::filesystem::path(std::getenv("XDG_DATA_HOME")) / "blastirc/state.sqlite3";
        assert(sqlite3_open(database.c_str(), &fault) == SQLITE_OK);
        assert(sqlite3_exec(fault,
                            "CREATE TRIGGER reject_sparse_around BEFORE INSERT ON messages "
                            "WHEN NEW.id='sparse-failure' BEGIN "
                            "SELECT RAISE(ABORT,'sparse around rejected'); END;",
                            nullptr, nullptr, nullptr) == SQLITE_OK);
        Application::HistoryPage page;
        page.server = room.server;
        page.buffer = room.id;
        page.request.target = room.target;
        page.request.direction = HistoryDirection::Around;
        for (const auto &id : {"sparse-5", "sparse-new", "sparse-failure"}) {
            Event row;
            row.type = id == string("sparse-5") ? "reaction" : "message";
            row.server = room.server;
            row.target = room.target;
            row.id = id;
            row.nick = "Other";
            row.text = id == string("sparse-5") ? "+1" : "page change must roll back";
            row.time =
                id == string("sparse-5") ? "2026-11-01T05:00:00.000Z" : "2026-11-01T06:00:00.000Z";
            row.history = true;
            page.events.push_back(std::move(row));
        }
        Event end;
        end.tags = {{"direction", "around"},         {"count", "3"},
                    {"oldest-id", "sparse-5"},       {"oldest-time", "2026-11-01T05:00:00.000Z"},
                    {"newest-id", "sparse-failure"}, {"newest-time", "2026-11-01T06:00:00.000Z"}};
        bool failed = false;
        try {
            app.applyHistoryPage(page, end);
        } catch (const std::exception &) {
            failed = true;
        }
        assert(failed && room.messages.size() == originalIds.size() &&
               app.messageCount_ == originalIds.size());
        for (size_t i = 0; i < originalIds.size(); ++i) {
            assert(room.messages[i].id == originalIds[i]);
            assert(room.messages[i].text == app.storage_.message(room.id, originalIds[i])->text);
            assert(room.messages[i].reactions.empty() &&
                   app.storage_.message(room.id, originalIds[i])->reactions.empty());
            const auto *resolved = sourceMessage(app.model, room.id, originalIds[i]);
            assert(resolved && resolved->id == originalIds[i]);
        }
        assert(app.model.activeBuffer == room.id &&
               app.model.pendingSourceJump.revision == anchor.revision &&
               app.model.pendingSourceJump.buffer == anchor.buffer &&
               app.model.pendingSourceJump.message == anchor.message);
        assert(!app.storage_.message(room.id, "sparse-new") &&
               !app.storage_.message(room.id, "sparse-failure"));
        assert(app.storage_.message(room.id, "sparse-5")->text == "original text 5");
        assert(sqlite3_exec(fault, "DROP TRIGGER reject_sparse_around", nullptr, nullptr,
                            nullptr) == SQLITE_OK);
        sqlite3_close(fault);
    }
    static Event tiedHistoryRow(const Buffer &room, const string &id) {
        Event row;
        row.type = "message";
        row.server = room.server;
        row.target = room.target;
        row.nick = "Other";
        row.id = id;
        row.tags["msgid"] = id;
        row.text = "same-time wire row " + id;
        row.time = "2026-11-01T00:00:00.123456789Z";
        row.timestamp = 1793491200;
        row.history = true;
        return row;
    }
    template <class Rows>
    static void assertHistoryIds(const Rows &rows, const vector<string> &expected) {
        assert(rows.size() == expected.size());
        for (size_t i = 0; i < expected.size(); ++i)
            assert(rows[i].id == expected[i]);
    }
    static void completeTiedHistory(Application &app, Buffer &room, const vector<Event> &rows,
                                    HistoryDirection direction, const string &owner,
                                    HistoryCursor first = {}) {
        Application::HistoryPage page;
        page.server = room.server;
        page.buffer = room.id;
        page.session = app.model.networks[room.server].sessionGeneration;
        page.request.target = room.target;
        page.request.requestId = owner;
        page.request.direction = direction;
        page.request.first = std::move(first);
        app.historyPages_[owner] = std::move(page);
        app.activeHistory_[owner] = room.id;
        vector<string> before;
        map<string, std::pair<int64_t, int64_t>> identities;
        for (const auto &row : room.messages) {
            before.push_back(row.id);
            identities.emplace(row.id, std::pair{row.storageSequence, row.historyOrder});
        }
        const auto count = app.messageCount_;
        const auto oldest = room.historyState.oldest;
        const auto newest = room.historyState.newest;
        vector<string> primary;
        for (auto row : rows) {
            if (!row.tags.contains("draft/chathistory-context"))
                primary.push_back(row.id);
            row.tags["history-owner"] = owner;
            app.reduce(row);
            // Even a complete-looking stream is invisible until its owning end.
            assertHistoryIds(room.messages, before);
            assert(app.messageCount_ == count && room.historyState.oldest == oldest &&
                   room.historyState.newest == newest);
            for (const auto &loaded : room.messages) {
                const auto durable = app.storage_.message(room.id, loaded.id);
                const auto identity = identities.at(loaded.id);
                assert(loaded.storageSequence == identity.first &&
                       loaded.historyOrder == identity.second);
                if (identity.first > 0)
                    assert(durable && durable->storageSequence == identity.first &&
                           durable->historyOrder == identity.second);
                else
                    assert(!durable);
            }
            for (const auto &incoming : rows)
                if (!identities.contains(incoming.id))
                    assert(!app.storage_.message(room.id, incoming.id));
        }
        assert(!primary.empty());
        Event end;
        end.type = "history_end";
        end.server = room.server;
        end.target = room.target;
        end.label = owner;
        end.tags = {{"history-owner", owner},
                    {"direction", direction == HistoryDirection::Before   ? "before"
                                  : direction == HistoryDirection::Around ? "around"
                                                                          : "latest"},
                    {"count", std::to_string(primary.size())},
                    {"oldest-id", primary.front()},
                    {"newest-id", primary.back()},
                    {"oldest-time", rows.front().time},
                    {"newest-time", rows.back().time}};
        app.reduce(end);
        assert(!app.historyPages_.contains(owner));
    }
    static void unknownTimeAroundPreservesNativeOrder() {
        Application app;
        app.model = Model{};
        app.model.settings.retentionDays = 0;
        app.storage_.prune(app.model.settings);
        auto &room = app.buffer("unknown-time-around-order", "#room");
        auto first = tiedHistoryRow(room, "z-original");
        auto last = tiedHistoryRow(room, "a-original");
        first.time = last.time = "2026-11-01T00:00:00.000Z";
        for (const auto &event : {first, last}) {
            Message row;
            row.id = event.id;
            row.nick = event.nick;
            row.text = event.text;
            row.time = event.time;
            row.timestamp = event.timestamp;
            app.storage_.append(room.id, row);
            app.insert(room, *app.storage_.message(room.id, row.id), false);
        }
        auto unknown = tiedHistoryRow(room, "m-unknown");
        unknown.time.clear(); // Numeric receipt seconds are not a server-time tie proof.
        completeTiedHistory(app, room, {unknown}, HistoryDirection::Around,
                            "unknown-time-owned-around");
        const vector<string> native{first.id, last.id, unknown.id};
        assertHistoryIds(room.messages, native);
        assertHistoryIds(app.storage_.history(room.id, -1), native);
        const auto untimed = app.storage_.message(room.id, unknown.id);
        assert(untimed && untimed->time.empty() && untimed->timestamp == unknown.timestamp);
        // Receipt-time coincidence must not confirm an ordering that prevents a
        // subsequent authoritative equal-server-time response from ordering its rows.
        completeTiedHistory(app, room, {last, first}, HistoryDirection::Latest,
                            "valid-time-after-unknown-around");
        const vector<string> authoritative{last.id, first.id, unknown.id};
        assertHistoryIds(room.messages, authoritative);
        Storage reopened;
        assertHistoryIds(reopened.history(room.id, -1), authoritative);
        assert(reopened.message(room.id, unknown.id)->time.empty());
    }
    static void historyReconciliationPreservesUnrelatedGhostPositions() {
        Application app;
        app.model = Model{};
        app.model.settings.retentionDays = 0;
        app.storage_.prune(app.model.settings);
        auto &room = app.buffer("history-unrelated-ghost-order", "#room");
        const vector<string> ghosts{"z-native-first", "a-native-second"};
        const vector<int64_t> ranks{9, 3};
        // Echo time correction can leave native positions intentionally different
        // from rank order. These model-only rows are not part of this response.
        for (size_t i = 0; i < ghosts.size(); ++i) {
            Message ghost;
            ghost.id = ghosts[i];
            ghost.label = "queued-" + ghosts[i];
            ghost.nick = "Me";
            ghost.text = "untouched optimistic row";
            ghost.time = "2026-10-31T00:00:00.123456789Z";
            ghost.timestamp = ghost.orderingTimestamp = 1793404800;
            ghost.historyOrder = ranks[i];
            ghost.own = ghost.pending = true;
            ghost.sendStatus = SendStatus::Queued;
            room.messages.push_back(std::move(ghost));
        }
        app.messageCount_ = ghosts.size();
        auto first = tiedHistoryRow(room, "q-target-first");
        auto last = tiedHistoryRow(room, "b-target-last");
        for (const auto &event : {first, last}) {
            Message row;
            row.id = event.id;
            row.nick = event.nick;
            row.text = event.text;
            row.time = event.time;
            row.timestamp = event.timestamp;
            app.storage_.append(room.id, row);
            app.insert(room, *app.storage_.message(room.id, row.id), false);
        }
        assertHistoryIds(room.messages, {ghosts[0], ghosts[1], first.id, last.id});
        completeTiedHistory(app, room, {last, first}, HistoryDirection::Latest,
                            "reconcile-only-returned-cohort");
        assertHistoryIds(room.messages, {ghosts[0], ghosts[1], last.id, first.id});
        assertHistoryIds(app.storage_.history(room.id, -1), {last.id, first.id});
        for (size_t i = 0; i < ghosts.size(); ++i) {
            const auto &row = room.messages[i];
            assert(row.id == ghosts[i] && row.label == "queued-" + ghosts[i] && row.nick == "Me" &&
                   row.text == "untouched optimistic row" &&
                   row.time == "2026-10-31T00:00:00.123456789Z" && row.timestamp == 1793404800 &&
                   row.orderingTimestamp == 1793404800 && row.historyOrder == ranks[i] &&
                   row.storageSequence == 0 && row.own && row.pending && !row.history &&
                   row.sendStatus == SendStatus::Queued);
            assert(!app.storage_.message(room.id, ghosts[i]));
        }
    }
    static void sameTimestampLatestThenOwnedBefore() {
        Application app;
        app.model = Model{};
        app.model.settings.retentionDays = 0;
        app.storage_.prune(app.model.settings);
        auto &room = app.buffer("same-time-native65", "#room");
        // Explicit wire chronology (00..64), deliberately unrelated to ID/text ordering.
        const vector<string> expected{
            "rQ", "aV", "zD", "bK", "yH", "cP", "xB", "dN", "wF", "eT", "vJ", "fR", "uL",
            "gX", "tC", "hZ", "sA", "iW", "Rj", "Ak", "Zl", "Bm", "Yn", "Co", "Xp", "Dq",
            "Wr", "Es", "Vt", "Fu", "Uv", "Gw", "Tx", "Hy", "Sz", "I0", "q9", "J1", "p8",
            "K2", "o7", "L3", "n6", "M4", "m5", "N5", "l4", "O6", "k3", "P7", "j2", "Q8",
            "i1", "S9", "h0", "T0", "g9", "U1", "f8", "V2", "e7", "W3", "d6", "X4", "c5"};
        vector<Event> latest, before;
        for (size_t i = 0; i < expected.size(); ++i)
            (i < 17 ? before : latest).push_back(tiedHistoryRow(room, expected[i]));
        completeTiedHistory(app, room, latest, HistoryDirection::Latest, "same-time-latest");
        const vector<string> tail(expected.begin() + 17, expected.end());
        assertHistoryIds(room.messages, tail);
        assertHistoryIds(app.storage_.history(room.id, -1), tail);
        map<string, int64_t> originalSequences;
        for (const auto &row : room.messages) {
            assert(row.storageSequence > 0);
            originalSequences.emplace(row.id, row.storageSequence);
        }
        completeTiedHistory(app, room, before, HistoryDirection::Before, "same-time-demand",
                            {tail.front(), latest.front().time});
        const auto assertRows = [&](const auto &rows) {
            assertHistoryIds(rows, expected);
            for (const auto &row : rows) {
                assert(row.time == latest.front().time && row.timestamp == 1793491200);
                if (originalSequences.contains(row.id))
                    assert(row.storageSequence == originalSequences.at(row.id));
            }
        };
        assertRows(room.messages);
        assertRows(app.storage_.history(room.id, -1));
        Storage reopened;
        assertRows(reopened.history(room.id, -1));
        // Ordinary local-first scrolling must use rank, not insertion seq, at this tie.
        room.messages.clear();
        room.sourceMessageIndex.clear();
        app.messageCount_ = 0;
        auto retainedTail = reopened.history(room.id, 48);
        assertHistoryIds(retainedTail, tail);
        for (auto &row : retainedTail)
            app.insert(room, std::move(row), false);
        app.action({"history", room.id, {}, {}});
        assertRows(room.messages);
        assert(app.historyPages_.empty() && app.messageCount_ == expected.size());
        // Older-first local insertion is a separate comparator path: every prefix row
        // has a later SQL identity, but an earlier durable tie rank than the cached tail.
        room.messages.clear();
        room.sourceMessageIndex.clear();
        app.messageCount_ = 0;
        auto prefix = reopened.history(room.id, -1, tail.front());
        assertHistoryIds(prefix, vector<string>(expected.begin(), expected.begin() + 17));
        for (auto &row : prefix)
            app.insert(room, std::move(row), false);
        retainedTail = reopened.history(room.id, 48);
        for (auto &row : retainedTail)
            app.insert(room, std::move(row), false);
        assertRows(room.messages);
        for (const auto &id : expected) {
            const auto *resolved = sourceMessage(app.model, room.id, id);
            assert(resolved && resolved->id == id);
        }
    }
    static void sameTimestampCanonicalContextAndConfirmedOverlap() {
        Application app;
        app.model = Model{};
        app.model.settings.retentionDays = 0;
        app.storage_.prune(app.model.settings);
        auto &room = app.buffer("same-time-context-order", "#room");
        auto last = tiedHistoryRow(room, "a-last");
        auto first = tiedHistoryRow(room, "z-first");
        auto own = tiedHistoryRow(room, "m-own-context");
        own.nick = "Me";
        own.own = true;
        // Seed canonical existing rows in the opposite arrival order, unconfirmed
        // as a page order; context and already-existing own rows still anchor it.
        for (const auto &event : {last, own}) {
            Message row;
            row.id = event.id;
            row.nick = event.nick;
            row.text = event.text;
            row.time = event.time;
            row.timestamp = event.timestamp;
            row.own = event.own;
            app.storage_.append(room.id, row);
            app.insert(room, *app.storage_.message(room.id, row.id), false);
        }
        const auto ownSeq = app.storage_.message(room.id, own.id)->storageSequence;
        own.tags["draft/chathistory-context"] = "1";
        completeTiedHistory(app, room, {first, own, last}, HistoryDirection::Latest,
                            "canonical-context");
        const vector<string> expected{first.id, own.id, last.id};
        assertHistoryIds(room.messages, expected);
        assertHistoryIds(app.storage_.history(room.id, -1), expected);
        assert(app.storage_.message(room.id, own.id)->storageSequence == ownSeq);
        // A contradictory overlapping response cannot reverse confirmed source order.
        completeTiedHistory(app, room, {last, own, first}, HistoryDirection::Latest,
                            "contradictory-confirmed-overlap");
        assertHistoryIds(room.messages, expected);
        Storage reopened;
        assertHistoryIds(reopened.history(room.id, -1), expected);
    }
    static void materializedOwnHistoryKeepsConfirmedNativeNeighbors() {
        Application app;
        app.model = Model{};
        app.model.settings.retentionDays = 0;
        app.storage_.prune(app.model.settings);
        auto &room = app.buffer("materialized-own-native-neighbors", "#room");
        app.model.networks[room.server].nick = "Me";
        auto first = tiedHistoryRow(room, "z-confirmed-first");
        auto last = tiedHistoryRow(room, "a-confirmed-last");
        completeTiedHistory(app, room, {first, last}, HistoryDirection::Latest,
                            "confirm-native-neighbors");
        const auto firstSeq = room.messages.front().storageSequence;
        const auto lastSeq = room.messages.back().storageSequence;
        Message optimistic;
        optimistic.id = "local:materialized-middle";
        optimistic.label = "materialized-middle-label";
        optimistic.nick = "Me";
        optimistic.text = "own middle row";
        optimistic.time = first.time;
        optimistic.timestamp = optimistic.orderingTimestamp = first.timestamp;
        optimistic.own = optimistic.pending = true;
        optimistic.sendStatus = SendStatus::Queued;
        room.messages.insert(room.messages.begin() + 1, std::move(optimistic));
        room.sourceMessageIndex.clear();
        ++app.messageCount_;
        auto middle = tiedHistoryRow(room, "m-canonical-middle");
        middle.nick = "Me";
        middle.text = "own middle row";
        middle.label = "materialized-middle-label";
        middle.own = true;
        completeTiedHistory(app, room, {middle}, HistoryDirection::Around,
                            "materialize-between-confirmed-neighbors");
        const vector<string> expected{first.id, middle.id, last.id};
        assertHistoryIds(room.messages, expected);
        Storage reopened;
        assertHistoryIds(reopened.history(room.id, -1), expected);
        assert(room.messages.front().storageSequence == firstSeq &&
               room.messages.back().storageSequence == lastSeq &&
               room.messages[1].storageSequence > 0 &&
               room.messages[1].sendStatus == SendStatus::Confirmed &&
               room.messages[1].label == middle.label);
        completeTiedHistory(app, room, {last, middle, first}, HistoryDirection::Latest,
                            "preserve-materialized-confirmed-source-order");
        assertHistoryIds(room.messages, expected);
        assertHistoryIds(reopened.history(room.id, -1), expected);
    }
    static void sameTimestampRankReconciliationRollback() {
        Application app;
        app.model = Model{};
        app.model.settings.retentionDays = 0;
        app.storage_.prune(app.model.settings);
        auto &room = app.buffer("same-time-rank-rollback", "#room");
        auto middle = tiedHistoryRow(room, "q-middle");
        auto last = tiedHistoryRow(room, "b-last");
        completeTiedHistory(app, room, {middle, last}, HistoryDirection::Latest, "rank-seed");
        const vector<string> original{middle.id, last.id};
        map<string, std::pair<int64_t, int64_t>> identities;
        for (const auto &row : room.messages)
            identities.emplace(row.id, std::pair{row.storageSequence, row.historyOrder});
        const auto state = room.historyState;
        const auto count = app.messageCount_;
        sqlite3 *fault = nullptr;
        const auto database =
            std::filesystem::path(std::getenv("XDG_DATA_HOME")) / "blastirc/state.sqlite3";
        assert(sqlite3_open(database.c_str(), &fault) == SQLITE_OK);
        assert(
            sqlite3_exec(fault,
                         "CREATE TRIGGER reject_reconciled_history BEFORE INSERT ON history_state "
                         "WHEN NEW.buffer='same-time-rank-rollback/#room' "
                         "AND EXISTS(SELECT 1 FROM messages WHERE buffer=NEW.buffer "
                         "AND id='z-first' AND tie_order=0) "
                         "AND EXISTS(SELECT 1 FROM messages WHERE buffer=NEW.buffer "
                         "AND id='q-middle' AND tie_order=1) BEGIN "
                         "SELECT RAISE(ABORT,'rank reconciliation rejected'); END;",
                         nullptr, nullptr, nullptr) == SQLITE_OK);
        Application::HistoryPage page;
        page.server = room.server;
        page.buffer = room.id;
        page.request.target = room.target;
        page.request.direction = HistoryDirection::Before;
        page.request.first = {middle.id, middle.time};
        auto first = tiedHistoryRow(room, "z-first");
        page.events = {first};
        Event end;
        end.tags = {{"direction", "before"},     {"count", "1"},
                    {"oldest-id", first.id},     {"newest-id", first.id},
                    {"oldest-time", first.time}, {"newest-time", first.time}};
        bool failed = false;
        try {
            app.applyHistoryPage(page, end);
        } catch (const std::exception &error) {
            failed = string(error.what()).find("rank reconciliation rejected") != string::npos;
        }
        assert(failed && app.messageCount_ == count);
        const auto assertRestored = [&](const auto &rows) {
            assertHistoryIds(rows, original);
            for (const auto &row : rows) {
                const auto identity = identities.at(row.id);
                assert(row.storageSequence == identity.first &&
                       row.historyOrder == identity.second && row.time == middle.time &&
                       row.timestamp == middle.timestamp);
            }
        };
        assertRestored(room.messages);
        assertRestored(app.storage_.history(room.id, -1));
        assert(!app.storage_.message(room.id, first.id));
        assert(room.historyState.oldest == state.oldest &&
               room.historyState.newest == state.newest &&
               room.historyState.gaps.size() == state.gaps.size());
        Storage rolledBack;
        assertRestored(rolledBack.history(room.id, -1));
        Buffer durableState;
        durableState.id = room.id;
        rolledBack.loadHistoryState(durableState);
        assert(durableState.historyState.oldest == state.oldest &&
               durableState.historyState.newest == state.newest &&
               durableState.historyState.gaps.size() == state.gaps.size());
        for (const auto &id : original)
            assert(sourceMessage(app.model, room.id, id));
        assert(!sourceMessage(app.model, room.id, first.id));
        assert(sqlite3_exec(fault, "DROP TRIGGER reject_reconciled_history", nullptr, nullptr,
                            nullptr) == SQLITE_OK);
        sqlite3_close(fault);
        app.applyHistoryPage(page, end);
        const vector<string> expected{first.id, middle.id, last.id};
        assertHistoryIds(room.messages, expected);
        Storage reopened;
        assertHistoryIds(reopened.history(room.id, -1), expected);
        for (const auto &id : original)
            assert(app.storage_.message(room.id, id)->storageSequence == identities.at(id).first);
    }
    static void historyWriterRollback() {
        Application app;
        app.model = Model{};
        app.model.settings.retentionDays = 0;
        auto &room = app.buffer("history-writer-rollback", "Peer", "query");
        Message anchor;
        anchor.id = "writer-anchor";
        anchor.text = "cached anchor";
        anchor.time = "2026-11-01T00:00:00.000Z";
        anchor.timestamp = 1793491200;
        app.storage_.append(room.id, anchor);
        room.messages.push_back(*app.storage_.message(room.id, anchor.id));
        app.messageCount_ = 1;
        room.historyState.oldest = room.historyState.newest = {anchor.id, anchor.time};
        HistoryGap existing;
        existing.older = {"older-anchor", "2026-10-31T00:00:00.000Z"};
        existing.newer = {anchor.id, anchor.time};
        room.historyState.gaps.push_back(existing);
        app.storage_.saveHistoryState(room);
        const auto before = room.historyState;
        sqlite3 *fault = nullptr;
        const auto database =
            std::filesystem::path(std::getenv("XDG_DATA_HOME")) / "blastirc/state.sqlite3";
        assert(sqlite3_open(database.c_str(), &fault) == SQLITE_OK);
        assert(sqlite3_exec(fault,
                            "CREATE TRIGGER reject_history_page BEFORE INSERT ON messages "
                            "WHEN NEW.id='writer-second' BEGIN "
                            "SELECT RAISE(ABORT,'history writer rejected'); END;",
                            nullptr, nullptr, nullptr) == SQLITE_OK);
        const auto complete = [&] {
            const string label = "writer-page";
            Application::HistoryPage page;
            page.server = room.server;
            page.buffer = room.id;
            page.session = app.model.networks[room.server].sessionGeneration;
            page.request.target = room.target;
            page.request.requestId = label;
            page.request.direction = HistoryDirection::Latest;
            for (const auto &id : {"writer-first", "writer-second"}) {
                Event message;
                message.type = "message";
                message.server = room.server;
                message.target = room.target;
                message.label = label;
                message.id = id;
                message.nick = "Other";
                message.text = "completed history page";
                message.time = id == string("writer-first") ? "2026-11-02T00:00:00.000Z"
                                                            : "2026-11-02T01:00:00.000Z";
                message.history = true;
                page.events.push_back(std::move(message));
            }
            app.historyPages_[label] = std::move(page);
            app.activeHistory_[label] = room.id;
            room.loading = true;
            Event end;
            end.type = "history_end";
            end.server = room.server;
            end.target = room.target;
            end.label = label;
            end.tags = {{"direction", "latest"},
                        {"count", "2"},
                        {"limit", "50"},
                        {"oldest-id", "writer-first"},
                        {"newest-id", "writer-second"},
                        {"oldest-time", "2026-11-02T00:00:00.000Z"},
                        {"newest-time", "2026-11-02T01:00:00.000Z"}};
            app.reduce(end);
        };
        complete();
        assert(!room.loading && room.messages.size() == 1 && room.messages.front().id == anchor.id);
        const auto log = std::find_if(
            app.model.buffers.begin(), app.model.buffers.end(), [&](const auto &entry) {
                return entry.second.server == room.server && entry.second.kind == "server";
            });
        assert(log != app.model.buffers.end() && log->second.messages.size() == 1);
        const auto &failure = log->second.messages.front();
        assert(failure.presence && failure.text.find("history writer rejected") != string::npos);
        const string failureId = failure.id;
        const auto loadedCount = [&] {
            size_t count = 0;
            for (const auto &[id, b] : app.model.buffers)
                if (b.kind != "search" && b.kind != "mentions")
                    count += b.messages.size();
            return count;
        };
        assert(app.messageCount_ == loadedCount());
        assert(!app.storage_.message(room.id, "writer-first") &&
               !app.storage_.message(room.id, "writer-second"));
        const auto unchanged = [&](const HistoryState &state) {
            assert(state.oldest == before.oldest && state.newest == before.newest &&
                   state.beginning == before.beginning && state.gaps.size() == 1 &&
                   state.gaps.front().id == before.gaps.front().id &&
                   state.gaps.front().older == before.gaps.front().older &&
                   state.gaps.front().newer == before.gaps.front().newer);
        };
        unchanged(room.historyState);
        Buffer restored = room;
        restored.historyState = {};
        app.storage_.loadHistoryState(restored);
        unchanged(restored.historyState);
        assert(sqlite3_exec(fault, "DROP TRIGGER reject_history_page", nullptr, nullptr, nullptr) ==
               SQLITE_OK);
        sqlite3_close(fault);
        complete(); // The same page remains retryable after a real writer failure.
        assert(!room.loading && room.messages.size() == 3);
        assert(log->second.messages.size() == 1 && log->second.messages.front().id == failureId &&
               log->second.messages.front().presence &&
               log->second.messages.front().text.find("history writer rejected") != string::npos);
        assert(app.messageCount_ == loadedCount());
        assert(app.storage_.message(room.id, "writer-first") &&
               app.storage_.message(room.id, "writer-second"));
        assert(room.historyState.newest.id == "writer-second");
    }
    static void historyWavePlanning() {
        Application app;
        app.model = Model{};
        app.savingAllowed_ = false;
        const string server = "history-wave-plan";
        app.channelTypes_[server] = "#";
        auto &session = app.historySessions_[server];
        vector<string> changed;
        for (int i = 0; i < 15; ++i) {
            auto &b = app.buffer(server, "Peer" + std::to_string(i), "query");
            changed.push_back(b.id);
            session.advertised[app.fold(server, b.target)] = {
                b.target, i == 0 ? "2026-11-02T00:00:00.000Z" : "2026-11-03T00:00:00.000Z"};
        }
        auto &viewed = app.model.buffers.at(changed.front());
        viewed.archived = viewed.muted = true;
        app.model.activeBuffer = viewed.id;
        app.model.settings.pinnedBuffers = {changed[2], changed[1]};
        auto &unchanged = app.buffer(server, "Current", "query");
        unchanged.historyState.newest = {"opaque-CURRENT", "2026-11-04T00:00:00.001Z"};
        session.advertised[app.fold(server, unchanged.target)] = {unchanged.target,
                                                                  "2026-11-04T00:00:00.999Z"};
        auto &unadvertised = app.buffer(server, "NoAdvert", "query");
        unadvertised.historyState.newest = {"opaque-cursor", "2026-11-01T00:00:00.000Z"};
        session.advertised["#unknown"] = {"#unknown", "2026-11-05T00:00:00.000Z"};
        app.planHistory(server);
        assert(session.visible.size() == 12 && session.background.size() == 3);
        assert(session.visible[0] == viewed.id && session.visible[1] == changed[2] &&
               session.visible[2] == changed[1]);
        set<string> planned(session.visible.begin(), session.visible.end());
        planned.insert(session.background.begin(), session.background.end());
        assert(planned == set<string>(changed.begin(), changed.end()));
        assert(unchanged.initialHistorySync == HistorySyncState::Idle &&
               unadvertised.initialHistorySync == HistorySyncState::Idle);
        for (const auto &id : session.background)
            assert(app.model.buffers.at(id).initialHistorySync == HistorySyncState::Idle);
        assert(session.nextTarget > app.model.now);
        // Older-target seeds only include advertised changed rooms.
        session.visible.clear();
        session.background.clear();
        for (const auto &id : changed)
            app.model.buffers.at(id).initialHistorySync = HistorySyncState::Idle;
        session.advertised.clear();
        session.advertised[app.fold(server, viewed.target)] = {viewed.target,
                                                               "2026-11-06T00:00:00.000Z"};
        auto &noCursor = app.buffer(server, "Cursorless", "query");
        app.planHistory(server, true);
        assert(session.seeds.size() == 1 && session.seeds.front() == viewed.id);
        assert(std::find(session.seeds.begin(), session.seeds.end(), noCursor.id) ==
               session.seeds.end());
    }
    static void historyBackfillCheckpoints() {
        Application app;
        app.model = Model{};
        app.model.settings.retentionDays = 0;
        for (int outcome = 0; outcome < 3; ++outcome) {
            const bool failed = outcome == 1, retired = outcome == 2;
            const bool checkpointed = !failed;
            const string server = "backfill-outcome-" + std::to_string(outcome);
            ServerConfig config;
            config.name = server;
            app.clients_[server] = std::make_unique<NetworkClient>(config, Identity{});
            app.model.networks[server].state = "Ready";
            app.model.networks[server].caps.insert("draft/chathistory");
            app.channelTypes_[server] = "#";
            auto &session = app.historySessions_[server];
            session.backfill = true;
            session.lower = "1970-01-01T00:00:00.000Z";
            session.upper = "2026-10-01T00:00:00.000Z";
            session.durable.watermark = "2026-11-01T00:00:00.000Z";
            session.durable.backfillUpper = session.upper;
            app.storage_.saveHistoryDiscovery(server, session.durable);
            Application::HistoryPage targets;
            targets.server = server;
            targets.request.direction = HistoryDirection::Targets;
            targets.request.limit = 100;
            Event advertised;
            advertised.type = "history_target";
            advertised.target = "OlderPeer";
            advertised.time = "2026-09-01T00:00:00.000Z";
            targets.events.push_back(advertised);
            if (retired) {
                advertised.target = "RemainingPeer";
                targets.events.push_back(advertised);
            }
            Event end;
            end.type = "history_end";
            end.server = server;
            end.tags = {{"direction", "targets"},
                        {"count", retired ? "2" : "1"},
                        {"end", "1"},
                        {"limit", "100"}};
            app.finishHistoryPage(std::move(targets), end, true, true);
            assert(session.seeds.size() == (retired ? 2u : 1u) &&
                   session.checkpoint == session.lower);
            app.scheduleHistory(); // The paced seed is not eligible yet.
            assert(app.storage_.historyDiscovery(server).backfillUpper ==
                   "2026-10-01T00:00:00.000Z");
            Application::HistoryPage seed;
            seed.server = server;
            seed.buffer = session.seeds.front();
            seed.request.target = app.model.buffers.at(seed.buffer).target;
            seed.seed = seed.backfillSeed = true;
            seed.request.initial = true;
            session.seeds.pop_front();
            app.historyPages_["held-seed"] = seed;
            app.scheduleHistory(); // Empty queue is not proof while its seed is in flight.
            assert(!app.storage_.historyDiscovery(server).backfillComplete);
            app.historyPages_.erase("held-seed");
            end.label = "held-seed";
            end.tags = {{"direction", "latest"}, {"count", "0"}};
            if (failed || retired) {
                end.tags["error"] = failed ? "invalid page parameters" : "target unavailable";
                end.tags["failure-code"] = failed ? "INVALID_PARAMS" : "INVALID_TARGET";
            }
            app.finishHistoryPage(std::move(seed), end, !failed && !retired, false);
            if (retired) {
                assert(!session.seedFailed && session.seeds.size() == 1);
                app.scheduleHistory();
                assert(!app.storage_.historyDiscovery(server).backfillComplete &&
                       app.storage_.historyDiscovery(server).backfillUpper ==
                           "2026-10-01T00:00:00.000Z");
                Application::HistoryPage remaining;
                remaining.server = server;
                remaining.buffer = session.seeds.front();
                remaining.seed = remaining.backfillSeed = true;
                remaining.request.initial = true;
                remaining.request.target = app.model.buffers.at(remaining.buffer).target;
                session.seeds.pop_front();
                end.label = "remaining-seed";
                end.tags = {{"direction", "latest"}, {"count", "0"}};
                app.finishHistoryPage(std::move(remaining), end, true, false);
            }
            app.scheduleHistory();
            const auto saved = app.storage_.historyDiscovery(server);
            assert(saved.watermark == "2026-11-01T00:00:00.000Z");
            assert(saved.backfillComplete == checkpointed);
            assert(saved.backfillUpper ==
                   (checkpointed ? "1970-01-01T00:00:00.000Z" : "2026-10-01T00:00:00.000Z"));
        }
    }
    static void saturatedDiscoveryFailsSafely() {
        Application app;
        app.model = Model{};
        const string server = "saturated-discovery";
        app.channelTypes_[server] = "#";
        auto &session = app.historySessions_[server];
        session.lower = "2026-10-01T00:00:00.000Z";
        session.upper = "2026-11-01T00:00:00.001Z";
        session.tieTime = "2026-11-01T00:00:00.000Z";
        session.tieTargets = {"peer"};
        session.durable.watermark = "2026-10-31T00:00:00.000Z";
        app.storage_.saveHistoryDiscovery(server, session.durable);
        Application::HistoryPage page;
        page.server = server;
        page.request.direction = HistoryDirection::Targets;
        page.request.limit = 2;
        Event target;
        target.type = "history_target";
        target.target = "Peer";
        target.time = session.tieTime;
        page.events = {target, target}; // Saturated repeated wire records, not dedup count.
        Event end;
        end.server = server;
        end.tags = {{"direction", "targets"}, {"count", "2"}, {"limit", "2"}};
        app.finishHistoryPage(std::move(page), end, true, false);
        assert(session.discoveryFailed && !session.discovering);
        assert(app.storage_.historyDiscovery(server).watermark == "2026-10-31T00:00:00.000Z");
    }
    static void discoveredLatestHistory() {
        RenameDrainPeer peer;
        Application app;
        app.model = Model{};
        app.model.settings.retentionDays = 0;
        Identity identity;
        identity.nick = "Me";
        ServerConfig server;
        server.name = "discovered-history";
        server.host = "127.0.0.1";
        server.port = peer.port;
        server.tls = false;
        app.model.identities = {identity};
        app.model.servers = {server};
        app.clients_[server.name] = std::make_unique<NetworkClient>(server, identity);
        auto &client = *app.clients_.at(server.name);
        auto &network = app.model.networks[server.name];
        auto &viewed = app.buffer(server.name, "Viewed", "query");
        auto &pinned = app.buffer(server.name, "Pinned", "query");
        auto &unchanged = app.buffer(server.name, "Unchanged", "query");
        auto &room = app.buffer(server.name, "#room");
        room.joined = true;
        viewed.muted = true; // Viewing wins over background mute priority.
        app.model.activeBuffer = viewed.id;
        app.model.settings.pinnedBuffers = {pinned.id};
        unchanged.historyState.newest = {"already-current", "2026-11-02T00:00:00.900Z"};
        Message cached;
        cached.id = "cached-tail";
        cached.text = "cached transcript remains visible";
        cached.time = "2026-11-01T00:00:00.000Z";
        cached.timestamp = 1793491200;
        app.storage_.append(room.id, cached);
        room.messages.push_back(cached);
        room.historyState.oldest = room.historyState.newest = {cached.id, cached.time};
        app.storage_.saveHistoryState(room);
        HistoryDiscovery checkpoint;
        checkpoint.watermark = "2026-10-31T00:00:00.000Z";
        checkpoint.backfillComplete = true; // Isolate reconnect discovery from older-target work.
        app.storage_.saveHistoryDiscovery(server.name, checkpoint);
        client.start();
        peer.accept();
        peer.expect("CAP", {"LS", "302"});
        peer.send(":server CAP * LS :batch draft/chathistory server-time");
        peer.expect("NICK", {identity.nick});
        peer.expect("USER", {identity.username, "0", "*", identity.realname});
        peer.expect("CAP", {"REQ", "batch draft/chathistory server-time"});
        peer.send(":server CAP Me ACK :batch draft/chathistory server-time");
        peer.expect("CAP", {"END"});
        peer.send(":server 001 Me :Welcome");
        // Reconnect membership can arrive before discovery admits LATEST. This
        // later live island must not replace the existing protocol newest boundary.
        peer.send("@msgid=new-live-join;time=2026-11-02T04:00:00.000Z :Me!u@h JOIN #room");
        peer.expect("WHO", {"#room"}); // The real own-JOIN path requests presence independently.
        peer.send(":server 315 Me #room :End of WHO list");
        peer.send(":server 366 Me #room :End of NAMES list");
        peer.send(":server 005 Me CHANTYPES=# CHATHISTORY=100 :supported");
        peer.send(":server 376 Me :End of MOTD");
        const auto drain = [&] {
            for (auto &event : client.poll())
                app.reduce(std::move(event));
            app.scheduleHistory();
        };
        const auto wait = [&](const auto &predicate) {
            const auto deadline = RenameDrainPeer::Clock::now() + std::chrono::seconds(5);
            do {
                drain();
                if (predicate())
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } while (RenameDrainPeer::Clock::now() < deadline);
            throw std::runtime_error("Discovered-history reduction deadline expired");
        };
        wait([&] { return network.state == "Ready" && !app.historyPages_.empty(); });
        const auto discovery = peer.read();
        assert(discovery.command == "CHATHISTORY" && discovery.params.size() == 4 &&
               discovery.params[0] == "TARGETS");
        assert(discovery.params[2] == "timestamp=2026-10-30T23:59:50.000Z");
        assert(viewed.messages.empty() && room.messages.size() == 2);
        // Duplicate advertisements normalize target identity and retain the newest time.
        peer.send(":server BATCH +targets draft/chathistory-targets");
        peer.send("@batch=targets :server CHATHISTORY TARGETS Viewed 2026-11-02T00:00:00.000Z");
        peer.send("@batch=targets :server CHATHISTORY TARGETS viewed 2026-11-01T00:00:00.000Z");
        peer.send("@batch=targets :server CHATHISTORY TARGETS Pinned 2026-11-02T01:00:00.000Z");
        peer.send("@batch=targets :server CHATHISTORY TARGETS #room 2026-11-02T02:00:00.000Z");
        peer.send("@batch=targets :server CHATHISTORY TARGETS Unchanged 2026-11-02T00:00:00.000Z");
        peer.send("@batch=targets :server CHATHISTORY TARGETS #unknown 2026-11-02T03:00:00.000Z");
        peer.send(":server BATCH -targets");
        wait([&] {
            return app.historySessions_.at(server.name).upper == "2026-11-01T00:00:00.001Z";
        });
        assert(app.storage_.historyDiscovery(server.name).watermark == checkpoint.watermark);
        const auto continuation = peer.read();
        assert(continuation.command == "CHATHISTORY" && continuation.params.size() == 4 &&
               continuation.params[0] == "TARGETS" &&
               continuation.params[1] == "timestamp=2026-11-01T00:00:00.001Z" &&
               continuation.params[2] == discovery.params[2]);
        // Replay an exclusive-boundary tie cohort once; a short repeat advances
        // to its exact time without claiming a fully enumerated timestamp cohort.
        peer.send(":server BATCH +tie draft/chathistory-targets");
        peer.send("@batch=tie :server CHATHISTORY TARGETS VIEWED 2026-11-01T00:00:00.000Z");
        peer.send(":server BATCH -tie");
        wait([&] {
            return app.historySessions_.at(server.name).upper == "2026-11-01T00:00:00.000Z";
        });
        const auto afterTie = peer.read();
        assert(afterTie.command == "CHATHISTORY" && afterTie.params.size() == 4 &&
               afterTie.params[0] == "TARGETS" &&
               afterTie.params[1] == "timestamp=2026-11-01T00:00:00.000Z" &&
               afterTie.params[2] == discovery.params[2]);
        assert(app.storage_.historyDiscovery(server.name).watermark == checkpoint.watermark);
        peer.send("@draft/chathistory-end :server BATCH +terminal draft/chathistory-targets");
        peer.send(":server BATCH -terminal");
        wait([&] { return app.historySessions_.at(server.name).planned; });
        assert(app.storage_.historyDiscovery(server.name).watermark == checkpoint.watermark);
        peer.expect("CHATHISTORY", {"LATEST", "Viewed", "*", "50"});
        assert(!viewed.loading && unchanged.initialHistorySync == HistorySyncState::Idle);
        assert(std::none_of(app.model.buffers.begin(), app.model.buffers.end(),
                            [](const auto &entry) { return entry.second.target == "#unknown"; }));
        peer.send(":server BATCH +viewed chathistory Viewed");
        peer.send(":server BATCH -viewed");
        wait([&] { return viewed.initialHistorySync == HistorySyncState::Idle; });
        peer.expect("CHATHISTORY", {"LATEST", "Pinned", "*", "50"});
        peer.send(":server BATCH +pinned chathistory Pinned");
        peer.send(":server BATCH -pinned");
        wait([&] { return pinned.initialHistorySync == HistorySyncState::Idle; });
        // Cached tails still fetch one newest page, never a connect-time AFTER chain.
        peer.expect("CHATHISTORY", {"LATEST", "#room", "*", "50"});
        peer.send(":server BATCH +latest chathistory #room");
        peer.send("@batch=latest;msgid=new-tail;time=2026-11-02T02:00:00.000Z "
                  ":Other!u@h PRIVMSG #room :newest page");
        // The same live JOIN appears in the newest page. Its overlap proves
        // nothing about the offline interval before this later island.
        peer.send("@batch=latest;msgid=new-live-join;time=2026-11-02T04:00:00.000Z "
                  ":Me!u@h JOIN #room");
        peer.fence();
        drain();
        assert(room.messages.size() == 2 && !app.storage_.message(room.id, "new-tail").has_value());
        peer.send(":server BATCH -latest");
        wait([&] { return room.initialHistorySync == HistorySyncState::Idle; });
        wait([&] {
            return app.storage_.historyDiscovery(server.name).watermark ==
                   "2026-11-02T04:00:00.000Z";
        });
        assert(room.messages.size() == 3 && !room.historyState.beginning &&
               room.historyState.gaps.size() == 1);
        const auto gap = room.historyState.gaps.front();
        assert(gap.id > 0 && gap.older.id == "cached-tail" && gap.newer.id == "new-tail");
        Buffer restored = room;
        restored.historyState = {};
        app.storage_.loadHistoryState(restored);
        assert(restored.historyState.gaps.size() == 1 &&
               restored.historyState.gaps.front().id == gap.id);
        // One demand quantum walks at most three progressing short pages.
        app.action({"history-gap", room.id, std::to_string(gap.id), {}});
        for (int i = 0; i < 3; ++i) {
            const string boundary = i ? "quantum-" + std::to_string(i - 1) : "new-tail";
            peer.expect("CHATHISTORY", {"BEFORE", "#room", "msgid=" + boundary, "50"});
            const string batch = "quantum" + std::to_string(i);
            const string id = "quantum-" + std::to_string(i);
            peer.send(":server BATCH +" + batch + " chathistory #room");
            peer.send("@batch=" + batch + ";msgid=" + id + ";time=2026-11-01T" +
                      std::to_string(23 - i) + ":00:00.000Z :Other!u@h PRIVMSG #room :gap page");
            peer.send(":server BATCH -" + batch);
            wait([&] { return app.storage_.message(room.id, id).has_value(); });
        }
        wait([&] { return !room.loading; });
        assert(room.historyState.gaps.size() == 1 && !room.historyState.beginning);
        peer.fence(); // No fourth BEFORE may precede the PONG.
        // A repeated boundary with no insertion/protocol progress stops this demand,
        // but keeps the durable gap available for another explicit attempt.
        app.action({"history-gap", room.id, std::to_string(gap.id), {}});
        peer.expect("CHATHISTORY", {"BEFORE", "#room", "msgid=quantum-2", "50"});
        peer.send(":server BATCH +noadvance chathistory #room");
        peer.send("@batch=noadvance;msgid=quantum-2;time=2026-11-01T21:00:00.000Z "
                  ":Other!u@h PRIVMSG #room :gap page");
        peer.send(":server BATCH -noadvance");
        wait([&] { return !room.loading; });
        assert(room.historyState.gaps.size() == 1 && !room.historyState.gaps.front().unavailable);
        peer.fence();
        // A full timestamp-selected page cannot safely skip equal-time siblings:
        // stop this quantum despite real inserts, leaving the gap retryable.
        peer.send(":server 005 Me MSGREFTYPES=timestamp :supported");
        wait([&] { return app.historyReferences_[server.name] == "timestamp"; });
        app.action({"history-gap", room.id, std::to_string(gap.id), {}});
        peer.expect("CHATHISTORY", {"BEFORE", "#room", "timestamp=2026-11-01T21:00:00.000Z", "50"});
        peer.send(":server BATCH +timestampfull chathistory #room");
        for (int i = 0; i < 50; ++i)
            peer.send("@batch=timestampfull;msgid=stamp-" + std::to_string(i) +
                      ";time=2026-11-01T20:00:00.000Z :Other!u@h PRIVMSG #room :equal time");
        peer.send(":server BATCH -timestampfull");
        wait([&] { return !room.loading; });
        assert(room.historyState.gaps.size() == 1 && !room.historyState.gaps.front().unavailable &&
               !room.historyState.beginning);
        peer.fence();
        peer.send(":server 005 Me MSGREFTYPES=msgid,timestamp :supported");
        wait([&] { return app.historyReferences_[server.name] == "msgid,timestamp"; });
        app.action({"history-gap", room.id, std::to_string(gap.id), {}});
        peer.expect("CHATHISTORY", {"BEFORE", "#room", "msgid=stamp-0", "50"});
        peer.send(":server BATCH +fill chathistory #room");
        peer.send("@batch=fill;msgid=cached-tail;time=2026-11-01T00:00:00.000Z "
                  ":Other!u@h PRIVMSG #room :cached transcript remains visible");
        peer.send("@batch=fill;msgid=middle;time=2026-11-01T12:00:00.000Z "
                  ":Other!u@h PRIVMSG #room :bridges the gap");
        peer.send(":server BATCH -fill");
        wait([&] { return !room.loading; });
        assert(room.historyState.gaps.empty() && room.messages.size() == 57 &&
               !room.historyState.beginning);
        app.storage_.loadHistoryState(restored);
        assert(restored.historyState.gaps.empty());
        // Failed correlated pages discard partial data and never alter discovery proof.
        app.requestHistory(room, "cached-tail", false);
        peer.expect("CHATHISTORY", {"BEFORE", "#room", "msgid=cached-tail", "50"});
        peer.send(":server BATCH +partial chathistory #room");
        peer.send("@batch=partial;msgid=must-not-commit;time=2026-10-31T12:00:00.000Z "
                  ":Other!u@h PRIVMSG #room :partial result");
        peer.send(":server FAIL CHATHISTORY INVALID_PARAMS #room :page rejected");
        wait([&] { return !room.loading; });
        assert(!app.storage_.message(room.id, "must-not-commit").has_value());
        assert(std::none_of(room.messages.begin(), room.messages.end(),
                            [](const auto &m) { return m.id == "must-not-commit"; }));
        assert(app.storage_.historyDiscovery(server.name).watermark == "2026-11-02T04:00:00.000Z");
        app.requestHistory(room, "cached-tail", false);
        peer.expect("CHATHISTORY", {"BEFORE", "#room", "msgid=cached-tail", "50"});
        assert(!app.historyPages_.empty());
        const string staleLabel = app.historyPages_.begin()->first;
        peer.send(":server BATCH +cancelled chathistory #room");
        peer.send("@batch=cancelled;msgid=cancelled-row;time=2026-10-31T11:00:00.000Z "
                  ":Other!u@h PRIVMSG #room :cancelled session");
        peer.fence();
        drain();
        assert(room.loading && !app.storage_.message(room.id, "cancelled-row").has_value());
        Event disconnected;
        disconnected.type = "state";
        disconnected.server = server.name;
        disconnected.text = "Disconnected";
        app.reduce(disconnected);
        assert(app.historyPages_.empty() && app.activeHistory_.empty() &&
               app.initialHistory_.empty() && app.historySessions_.empty());
        for (const auto &[id, b] : app.model.buffers)
            if (b.server == server.name)
                assert(!b.loading && b.initialHistorySync == HistorySyncState::Idle);
        Event staleEnd;
        staleEnd.type = "history_end";
        staleEnd.server = server.name;
        staleEnd.target = room.target;
        staleEnd.label = staleLabel;
        staleEnd.tags = {{"direction", "before"}, {"count", "1"}, {"end", "1"}};
        app.reduce(staleEnd);
        assert(!app.storage_.message(room.id, "cancelled-row").has_value() &&
               app.historyPages_.empty());
        assert(app.storage_.historyDiscovery(server.name).watermark == "2026-11-02T04:00:00.000Z");
    }
    static void run() {
        commandArgumentContext();
        repeatSearch();
        retainedSearchJump();
        correlatedSearchHistory();
        appOriginAfterSwitch();
        localOriginErrors();
        closedQueuedCommandOrigin();
        networkStatusErrors();
        sojuHierarchy();
        globalRead();
        historyReadyWithoutIsupport();
        zncReconnectPlaybackCursor();
        historyRenameContinuity();
        historyMutationTargetsAreNotPageCoverage();
        latestGapCanonicalPrimaryOverlap();
        latestGapUsesProtocolBoundary();
        prunedHistoryCannotReproveEndpoints();
        sparseAroundRollbackPreservesLoadedWindow();
        historyWriterRollback();
        sameTimestampLatestThenOwnedBefore();
        sameTimestampCanonicalContextAndConfirmedOverlap();
        sameTimestampRankReconciliationRollback();
        unknownTimeAroundPreservesNativeOrder();
        historyReconciliationPreservesUnrelatedGhostPositions();
        materializedOwnHistoryKeepsConfirmedNativeNeighbors();
        historyWavePlanning();
        historyBackfillCheckpoints();
        saturatedDiscoveryFailsSafely();
        discoveredLatestHistory();
        sojuReconnectCatalog();
        sendStatuses();
        avatars();
        messageRevisions();
        truthfulActivity();
        caseMappingFacts();
        foolPolicy();
        retainedMentions();
        chatControls();
        chatActivityPins();
        presenceInheritance();
        renameDuringDisconnect(false);
        renameDuringDisconnect(true);
        Application app;
        app.model = Model{};
        app.messageCount_ = 0;
        app.activity_.clear();
        app.model.now = 100;
        // Fixed server timestamps exercise reduction, not wall-clock retention.
        app.model.settings.retentionDays = 0;
        app.storage_.prune(app.model.settings);
        app.model.networks["test"].nick = "Me";
        auto &room = app.buffer("test", "#[Room]");
        app.model.activeBuffer = room.id;
        assert(&room == &app.buffer("test", "#{room}"));

        Event old;
        old.type = "message";
        old.server = "test";
        old.target = "#[Room]";
        old.nick = "Other";
        old.text = "untimed playback";
        old.id = "history-1";
        old.history = true;
        app.reduce(old);
        assert(room.messages.size() == 1 && room.messages.front().timestamp == 0);
        assert(app.model.effects.empty());
        app.reduce(old);
        assert(room.messages.size() == 1);

        Event send;
        send.type = "message";
        send.server = "test";
        send.target = "#[Room]";
        send.nick = "Me";
        send.text = "hello";
        send.id = "wire-local";
        send.label = "unique-label";
        send.reply = old.id;
        send.own = true;
        send.tags["pending"] = "1";
        app.reduce(send);
        assert(room.messages.size() == 2 && room.messages.back().pending);
        assert(room.messages.back().id.starts_with("local:"));
        room.messages.back().reactions["+1"] = {"Other"};
        room.messages.back().pending =
            false; // One-second timeout must not break late ack matching.
        app.store(room, room.messages.back(), true);
        room.readMarker = "local:" + room.messages.back().id;
        room.unread = 0;
        app.model.now = 102;
        Event echo = send;
        echo.tags.clear();
        echo.id = "server-message-id";
        echo.timestamp = 1770000000;
        echo.time = "2026-02-02T02:40:00Z";
        app.reduce(echo);
        assert(room.messages.size() == 2 && room.messages.back().id == echo.id);
        assert(!room.messages.back().pending && room.messages.back().timestamp == echo.timestamp);
        assert(room.readMarker == echo.time && room.unread == 0);
        auto durable = app.storage_.history(room.id, 200);
        assert(std::count_if(durable.begin(), durable.end(),
                             [](const auto &m) { return m.label == "unique-label"; }) == 1);
        const auto canonical = std::find_if(durable.begin(), durable.end(), [](const auto &m) {
            return m.label == "unique-label";
        });
        assert(canonical->id == echo.id && canonical->text == send.text &&
               canonical->reply == send.reply && canonical->time == echo.time &&
               canonical->timestamp == echo.timestamp && canonical->own && !canonical->pending &&
               canonical->reactions == room.messages.back().reactions);
        assert(canonical->reactions.at("+1") == vector<string>{"Other"});
        app.reduce(echo);
        assert(room.messages.size() == 2);

        // Timestamp-only bouncers keep opaque msgids, including on disk-loaded rows.
        Buffer cached;
        cached.id = "history/#cached";
        cached.server = "history";
        Message historyMessage;
        historyMessage.id = "2026-11-04T12:00:00Z";
        historyMessage.time = "2026-11-04T12:00:00.123Z";
        historyMessage.timestamp = 1793793600;
        app.storage_.append(cached.id, historyMessage);
        cached.messages.push_back(*app.storage_.message(cached.id, historyMessage.id));
        app.reviseMessages(cached);
        Event references;
        references.type = "network";
        references.server = cached.server;
        references.tags = {{"kind", "isupport"}, {"MSGREFTYPES", "timestamp"}};
        app.reduce(references);
        assert(app.historyCursor(cached, historyMessage.id) ==
               "timestamp=2026-11-04T12:00:00.123Z");
        assert(app.historyCursor(cached, "msgid=" + historyMessage.id) ==
               "timestamp=2026-11-04T12:00:00.123Z");
        assert(app.historyCursor(cached, "timestamp=2026-11-04T12:00:00.456Z") ==
               "timestamp=2026-11-04T12:00:00.456Z");
        assert(app.historyCursor(cached, "unknown-id") == "msgid=unknown-id");
        cached.messages.front().time.clear();
        assert(app.historyCursor(cached, historyMessage.id) == "msgid=" + historyMessage.id);
        cached.messages.front().time = historyMessage.time;
        references.tags["MSGREFTYPES"] = "timestamp,msgid";
        app.reduce(references);
        assert(app.historyCursor(cached, historyMessage.id) == "msgid=" + historyMessage.id);
        references.tags.erase("MSGREFTYPES");
        references.tags["-MSGREFTYPES"] = "";
        app.reduce(references);
        assert(app.historyCursor(cached, historyMessage.id) == "msgid=" + historyMessage.id);
        // Native ZNC playback uses timestamp cursors even without CHATHISTORY.
        ServerConfig znc;
        znc.name = "znc";
        znc.bouncer = "znc";
        app.model.servers.push_back(znc);
        auto &zncRoom = app.buffer(znc.name, "#playback");
        historyMessage.id = "opaque-znc-id";
        zncRoom.messages.push_back(historyMessage);
        app.reviseMessages(zncRoom);
        app.clients_[znc.name] = std::make_unique<NetworkClient>(znc, Identity{});
        app.requestHistory(zncRoom, "", true);
        assert(!zncRoom.loading && zncRoom.initialHistorySync == HistorySyncState::Idle);
        Event nativeCaps;
        nativeCaps.type = "caps";
        nativeCaps.server = znc.name;
        nativeCaps.params = {"znc.in/playback"};
        app.model.networks[znc.name].state = "Ready";
        zncRoom.joined = true;
        app.reduce(nativeCaps);
        assert(app.historyCursor(zncRoom, historyMessage.id) == "timestamp=" + historyMessage.time);
        app.requestHistory(zncRoom, "", true);
        assert(!zncRoom.loading && zncRoom.initialHistorySync == HistorySyncState::Queued);
        assert(zncRoom.historyState.gaps.empty()); // Native playback is not TARGETS discovery.
        app.clients_.erase(znc.name);              // This admission check never starts a transport.

        // 005 events are complete current snapshots, including token removals.
        Event support;
        support.type = "network";
        support.server = "support";
        support.tags = {{"kind", "isupport"},
                        {"CASEMAPPING", "ascii"},
                        {"CHANTYPES", "!"},
                        {"PREFIX", "(ov)!+"},
                        {"CHANMODES", "b,k,l,imnpst"},
                        {"CLIENTTAGDENY", "reply,draft/react"}};
        app.reduce(support);
        assert(app.fold(support.server, "[") == "[");
        assert(!app.channel(support.server, "#room"));
        assert(!app.model.networks[support.server].clientTagDeny.empty());
        auto &supported = app.buffer(support.server, "#room");
        supported.nicks.push_back({"Other", "", "", false, 0});
        support.tags = {{"kind", "isupport"}};
        app.reduce(support);
        assert(app.fold(support.server, "[") == "{");
        assert(app.channel(support.server, "#room"));
        assert(app.model.networks[support.server].clientTagDeny.empty());
        Event mode;
        mode.type = "network";
        mode.server = support.server;
        mode.tags = {{"kind", "mode"}, {"command", "MODE"}};
        mode.params = {"#room", "+o", "Other"};
        app.reduce(mode);
        assert(supported.nicks.front().prefix == "@");
        support.tags["CHANTYPES"] = "";
        app.reduce(support);
        assert(!app.channel(support.server, "#room"));

        // Backloaded rows within one second precede the loaded newer fraction.
        auto &preciseRoom = app.buffer("fraction", "#room");
        Event fraction = old;
        fraction.server = preciseRoom.server;
        fraction.target = preciseRoom.target;
        fraction.id = "newer-fraction";
        fraction.time = "2026-11-05T00:00:00.900Z";
        fraction.timestamp = 1793836800;
        fraction.history = false;
        app.reduce(fraction);
        fraction.id = "older-fraction";
        fraction.time = "2026-11-05T01:00:00.100+01:00";
        fraction.history = true;
        app.reduce(fraction);
        assert(preciseRoom.messages.front().id == "older-fraction" &&
               preciseRoom.messages.back().id == "newer-fraction");
        app.markRead(preciseRoom);
        assert(preciseRoom.readMarker == "2026-11-05T00:00:00.900Z");

        Event caseNick;
        caseNick.type = "nick";
        caseNick.server = "test";
        caseNick.nick = "Other";
        caseNick.text = "OTHER";
        room.nicks.push_back({"Other", "account", "@", true, 1});
        app.activity_[room.id + "\nother"].push_back(100);
        app.reduce(caseNick); // Case-only nick changes must not self-insert/erase activity deques.
        assert(room.nicks.front().name == "OTHER" && room.nicks.front().away);
        assert(app.activity_.at(room.id + "\nother").size() == 1);
        Event pastJoin;
        pastJoin.type = "join";
        pastJoin.server = "test";
        pastJoin.target = "#past";
        pastJoin.nick = "Me";
        pastJoin.history = true;
        app.reduce(pastJoin);
        auto &past = app.buffer("test", "#past");
        assert(!past.joined && past.nicks.empty());

        app.model.effects.clear();
        app.model.settings.workspace["visible_buffers"] = "elsewhere";
        Event mention = old;
        mention.history = false;
        mention.id = "mention-1";
        mention.text = "hi, mE!";
        app.reduce(mention);
        assert(std::count_if(app.model.effects.begin(), app.model.effects.end(),
                             [](const auto &e) { return e.type == "notify"; }) == 1);
        assert(std::none_of(app.model.effects.begin(), app.model.effects.end(),
                            [](const auto &e) { return e.type == "mention"; }));
        app.model.effects.clear();
        app.model.settings.fools.insert("OTHER");
        mention.id = "mention-2";
        app.reduce(mention);
        assert(app.model.effects.empty());

        app.model.settings.presence = PresenceMode::Hidden;
        Event join;
        join.type = "join";
        join.server = "test";
        join.target = room.target;
        join.nick = "Quiet";
        const auto beforeHidden = room.messages.size();
        app.reduce(join);
        assert(room.messages.size() == beforeHidden);
        app.model.settings.presence = PresenceMode::Smart;
        join.nick = "Newcomer";
        join.id = "newcomer-join";
        join.time = "2026-02-02T02:40:00Z";
        join.timestamp = 1770000000;
        app.reduce(join);
        assert(room.messages.size() == beforeHidden + 1);
        const auto &joined = room.messages.back();
        assert(joined.id == join.id && joined.nick == join.nick && joined.time == join.time &&
               joined.timestamp == join.timestamp && joined.presence &&
               joined.activity == ActivityKind::Join && !joined.own);
        Event speaking = mention;
        speaking.nick = "Newcomer";
        speaking.id = "newcomer-message";
        speaking.text = "hello";
        app.reduce(speaking);
        assert(room.messages.size() == beforeHidden + 2 &&
               room.messages[room.messages.size() - 2].id == join.id &&
               !room.messages.back().presence &&
               room.messages.back().activity == ActivityKind::None);

        const string unloaded = "old-unloaded";
        Message saved;
        saved.id = unloaded;
        saved.nick = "Newcomer";
        saved.text = "durable old message";
        app.storage_.append(room.id, saved);
        Event removal;
        removal.type = "redact";
        removal.server = "test";
        removal.target = room.target;
        removal.id = unloaded;
        app.reduce(removal);
        assert(app.storage_.message(room.id, unloaded)->deleted);

        const auto authoritative = app.messageCount_;
        app.action({"search", room.id, "hello", {}});
        assert(app.messageCount_ == authoritative);
        assert(!app.buffer("test", "*search*", "search").messages.empty());

        Event rejected = send;
        rejected.text = "not delivered";
        rejected.label = "rejected-label";
        app.reduce(rejected);
        room.draft = "new text typed while waiting";
        Event failure;
        failure.type = "error";
        failure.server = "test";
        failure.target = room.target;
        failure.label = rejected.label;
        failure.text = "Cannot send to channel";
        failure.tags["send-failed"] = "1";
        failure.tags["original-text"] = rejected.text;
        app.reduce(failure);
        assert(room.draft == rejected.text + "\nnew text typed while waiting");
        assert(std::none_of(room.messages.begin(), room.messages.end(),
                            [&](const auto &m) { return m.label == rejected.label; }));
        auto afterFailure = app.storage_.history(room.id, 200);
        assert(std::none_of(afterFailure.begin(), afterFailure.end(),
                            [&](const auto &m) { return m.label == rejected.label; }));

        // A real absent-client send rejection restores submission ahead of the current draft.
        room.draft = "newer offline draft";
        app.action({"send", room.id, "offline submission 猫✨", {}});
        assert(room.draft == "offline submission 猫✨\nnewer offline draft");
        const auto rejectedDraft = room.draft;
        app.action({"send", room.id, {}, {}});
        assert(room.draft == rejectedDraft);
        room.draft = "same submitted and newer text";
        app.action({"send", room.id, room.draft, {}});
        assert(room.draft == "same submitted and newer text\nsame submitted and newer text");
        room.draft.clear();
        app.command(room, "/me disconnected action 猫");
        assert(room.draft == "/me disconnected action 猫");
        app.command(room, "/msg OfflinePeer disconnected private text ✨");
        assert(app.buffer("test", "OfflinePeer").draft == "disconnected private text ✨");
        const auto messageCount = room.messages.size();
        const auto preferenceDraft = room.draft, currentReadMarker = room.readMarker;
        assert(app.model.settings.messageLayouts.empty());
        app.command(room, "/messagestyle irc");
        assert(app.model.settings.messageLayouts.at(room.id) == MessageLayout::Irc);
        Model preferenceReload;
        app.storage_.load(preferenceReload);
        assert(preferenceReload.settings.messageLayouts == app.model.settings.messageLayouts);
        app.command(room, "/messagestyle bubbles");
        assert(app.model.settings.messageLayouts.empty());
        app.command(room, "/messagestyle");
        assert(app.model.settings.messageLayouts.at(room.id) == MessageLayout::Irc);
        app.command(room, "/messagestyle");
        assert(app.model.settings.messageLayouts.empty());
        app.command(room, "/messagestyle stacked");
        assert(app.model.settings.messageLayouts.at(room.id) == MessageLayout::Stacked);
        app.storage_.load(preferenceReload);
        assert(preferenceReload.settings.messageLayouts == app.model.settings.messageLayouts);
        app.command(room, "/messagestyle");
        assert(app.model.settings.messageLayouts.empty());
        app.command(room, "/messagestyle stacked");
        app.command(room, "/messagestyle invalid");
        assert(app.model.settings.messageLayouts.at(room.id) == MessageLayout::Stacked);
        assert(room.messages.size() == messageCount + 1 && room.messages.back().presence &&
               room.messages.back().text == app.model.status && room.draft == preferenceDraft &&
               room.readMarker == currentReadMarker);

        auto &dm = app.buffer("test", "Alice", "query");
        dm.draft = "unfinished DM";
        Message personal;
        personal.id = "personal-message";
        personal.nick = "Alice";
        personal.text = "private history";
        app.insert(dm, personal);
        const auto oldDm = dm.id;
        app.model.activeBuffer = oldDm;
        app.model.settings.workspace["0"] = oldDm;
        Event renamed;
        renamed.type = "nick";
        renamed.server = "test";
        renamed.nick = "Alice";
        renamed.text = "Bob";
        app.reduce(renamed);
        assert(!app.model.buffers.contains(oldDm));
        auto &bob = app.buffer("test", "Bob");
        assert(bob.draft == "unfinished DM" && app.model.activeBuffer == bob.id);
        assert(app.model.settings.workspace["0"] == bob.id);
        assert(app.storage_.message(bob.id, personal.id));
        const auto bobMessages = bob.messages.size();
        Event replacement = old;
        replacement.history = false;
        replacement.target = "Me";
        replacement.nick = "Alice";
        replacement.id = "new-alice";
        replacement.text = "new person";
        app.reduce(replacement);
        assert(app.buffer("test", "Alice").messages.back().id == "new-alice");
        assert(bob.messages.size() == bobMessages);
        // Negotiated RENAME changes the identity, not the title to its reason.
        ServerConfig renamedServer;
        renamedServer.name = "rename";
        renamedServer.channels = {"#Old", "#New"};
        app.model.servers.push_back(renamedServer);
        auto &source = app.buffer(renamedServer.name, "#Old");
        auto &destination = app.buffer(renamedServer.name, "#New");
        source.joined = true;
        source.topic = "current live topic";
        source.nicks.push_back({"LiveMember", "account", "@", true, 10});
        source.typing["LiveMember"] = app.model.now;
        source.draft = "source draft";
        destination.draft = "destination draft";
        destination.topic = "stale topic";
        destination.nicks.push_back({"StaleMember", "", "", false, 0});
        Message channelHistory = personal;
        channelHistory.id = "source-history";
        app.insert(source, channelHistory);
        channelHistory.id = "destination-history";
        app.insert(destination, channelHistory);
        const string sourceId = source.id, destinationId = destination.id;
        app.model.activeBuffer = sourceId;
        app.model.settings.workspace["41"] = sourceId;
        app.model.settings.workspace["42"] = sourceId;
        app.model.settings.workspace["visible_buffers"] = sourceId;
        app.model.settings.workspace["last_buffer:rename"] = sourceId;
        app.model.settings.workspace["settings_feedback"] = sourceId;
        app.model.settings.messageLayouts[sourceId] = MessageLayout::Stacked;
        app.model.settings.messageLayouts[destinationId] = MessageLayout::Irc;
        app.model.settings.channelPresence[sourceId] = PresenceMode::All;
        app.model.actions.push_back({"history", sourceId, {}, {}});
        app.activeHistory_["rename-history"] = sourceId;
        app.requestedHistory_.insert(sourceId);
        app.storage_.queueSend(source, "rename-submission", "recoverable text");
        app.queuedSends_["rename-submission"] = {sourceId, "recoverable text", 1};
        Event channelPending = send;
        channelPending.server = renamedServer.name;
        channelPending.target = source.target;
        channelPending.label = "rename-submission";
        channelPending.tags["submission"] = channelPending.label;
        app.reduce(channelPending);
        bool identityDelivered = false;
        app.model.bufferRenamed = [&](const string &from, const string &to) {
            identityDelivered = true;
            assert(from == sourceId && to == destinationId);
            assert(!app.model.buffers.contains(from));
            const auto &migrated = app.model.buffers.at(to);
            assert(migrated.draft == "source draft\ndestination draft");
            assert(migrated.joined && migrated.topic == "current live topic");
            assert(migrated.nicks.size() == 1 && migrated.nicks.front().name == "LiveMember");
            assert(app.model.settings.workspace.at("41") == to &&
                   app.model.settings.workspace.at("42") == to);
            assert(app.activeHistory_.at("rename-history") == to);
            assert(app.queuedSends_.at("rename-submission").buffer == to);
            assert(app.model.servers.back().channels == vector<string>{"#New"});
            assert(app.storage_.message(to, "source-history") &&
                   app.storage_.message(to, "destination-history"));
        };
        Event channelRename;
        channelRename.type = "network";
        channelRename.server = renamedServer.name;
        channelRename.nick = "Operator";
        channelRename.target = "#Old";
        channelRename.params = {"#Old", "#New", ""};
        channelRename.tags = {{"kind", "rename"}, {"command", "RENAME"}};
        app.reduce(channelRename);
        assert(identityDelivered);
        auto &newRoom = app.model.buffers.at(destinationId);
        assert(newRoom.title == "#New" && newRoom.typing.contains("LiveMember"));
        assert(app.model.actions.back().buffer == destinationId &&
               app.requestedHistory_.contains(destinationId));
        assert(app.model.settings.channelPresence.at(destinationId) == PresenceMode::All);
        assert(app.model.settings.messageLayouts.at(destinationId) == MessageLayout::Stacked &&
               !app.model.settings.messageLayouts.contains(sourceId) &&
               app.model.settings.messageLayouts.at(room.id) == MessageLayout::Stacked);
        assert(app.model.settings.workspace.at("last_buffer:rename") == destinationId &&
               app.model.settings.workspace.at("settings_feedback") == sourceId);
        assert(app.model.settings.workspace.at("visible_buffers") == destinationId);
        assert(app.save());
        app.storage_.load(preferenceReload);
        assert(preferenceReload.settings.messageLayouts == app.model.settings.messageLayouts);
        assert(std::all_of(app.pending_.begin(), app.pending_.end(), [&](const auto &entry) {
            return entry.second.label != "rename-submission" ||
                   entry.second.buffer == destinationId;
        }));
        app.model.bufferRenamed = [&](const string &from, const string &to) {
            assert(from == destinationId && to == destinationId);
            assert(app.model.buffers.at(to).target == "#NEW");
        };
        channelRename.target = "#New";
        channelRename.params = {"#New", "#NEW", "Case change"};
        channelRename.text = "Case change";
        app.reduce(channelRename);
        assert(newRoom.draft == "source draft\ndestination draft" && newRoom.title == "#NEW" &&
               newRoom.joined);
        app.model.bufferRenamed = {};
        channelRename.history = true;
        channelRename.params = {"#NEW", "#Past", "Past rename"};
        channelRename.text = "Past rename";
        app.reduce(channelRename);
        assert(newRoom.target == "#NEW" && !app.model.buffers.contains("rename/#past"));

        // Closing after pending-history handoff must not require a live queued backup.
        auto &closing = app.buffer("test", "ClosePeer", "query");
        const string closingId = closing.id, closingTarget = closing.target;
        app.command(closing, "/messagestyle irc");
        Message unfinished;
        unfinished.id = app.localId();
        unfinished.label = "handed-off-close";
        unfinished.text = "unconfirmed submission";
        unfinished.own = unfinished.pending = true;
        app.insert(closing, unfinished);
        Message confirmed;
        confirmed.id = "confirmed-close-history";
        confirmed.text = "preserved confirmed history";
        confirmed.own = true;
        app.insert(closing, confirmed, true, {}, true);
        app.action({"close_buffer", closingId, {}, {}});
        assert(!app.model.buffers.contains(closingId));
        assert(!app.storage_.message(closingId, unfinished.id));
        assert(app.storage_.message(closingId, confirmed.id));
        Event lateFailure = failure;
        lateFailure.target = closingTarget;
        lateFailure.label = unfinished.label + ".part1";
        lateFailure.tags["original-text"] = "late unseen fragment";
        app.reduce(lateFailure);
        assert(!app.model.buffers.contains(closingId));
        assert(app.model.settings.messageLayouts.at(closingId) == MessageLayout::Irc);
        assert(app.save());
        app.storage_.load(preferenceReload);
        assert(preferenceReload.settings.messageLayouts.at(closingId) == MessageLayout::Irc &&
               !preferenceReload.buffers.contains(closingId));
        auto &reopened = app.buffer("test", closingTarget, "query");
        assert(reopened.id == closingId &&
               app.model.settings.messageLayouts.at(reopened.id) == MessageLayout::Irc);
        app.command(reopened, "/messagestyle bubbles");
        app.storage_.load(preferenceReload);
        assert(!preferenceReload.settings.messageLayouts.contains(closingId) &&
               preferenceReload.settings.messageLayouts.at(destinationId) ==
                   MessageLayout::Stacked);

        // A failed preference commit must not authorize deleting an imported layout.
        assert(app.save());
        const auto preferences =
            std::filesystem::path(std::getenv("XDG_CONFIG_HOME")) / "blastirc" / "settings.toml";
        const auto backup = preferences.string() + ".saved";
        std::filesystem::rename(preferences, backup);
        std::filesystem::create_directory(preferences);
        assert(!app.save());
        assert(std::filesystem::is_directory(preferences));
        // Save and Connect must not tear down a connection after a failed commit.
        const auto saveError = app.model.status;
        auto &connectionConfig = app.model.servers.back();
        connectionConfig.host = "127.0.0.1";
        connectionConfig.port = 1;
        connectionConfig.tls = false;
        if (app.model.identities.empty())
            app.model.identities.push_back(Identity{});
        connectionConfig.identity = app.model.identities.front().name;
        const string connectionName = connectionConfig.name;
        app.clients_[connectionName] =
            std::make_unique<NetworkClient>(connectionConfig, app.model.identities.front());
        auto *const unchangedClient = app.client(connectionName);
        unchangedClient->send(newRoom.target, "queued before failed Save and Connect", {},
                              "save-connect-preserved");
        auto &unchangedNetwork = app.model.networks[connectionName];
        unchangedNetwork.state = "Ready";
        unchangedNetwork.error = "existing upstream status";
        const auto currentCaps = unchangedNetwork.caps;
        const auto currentDraft = newRoom.draft;
        app.model.status = "before Save and Connect";
        const auto failedRequest = ++app.model.settingsSaveRequest;
        app.action({"save-connect", newRoom.id, connectionName, {}, failedRequest});
        assert(app.client(connectionName) == unchangedClient);
        assert(unchangedNetwork.state == "Ready" &&
               unchangedNetwork.error == "existing upstream status" &&
               unchangedNetwork.caps == currentCaps);
        assert(newRoom.joined && newRoom.draft == currentDraft);
        assert(app.model.status == saveError);
        assert(app.model.settingsSaveResult.requestId == failedRequest &&
               !app.model.settingsSaveResult.success &&
               app.model.settingsSaveResult.error == saveError);
        app.clients_.erase(connectionName); // Never started; no transport was contacted.
        std::filesystem::remove(preferences);
        std::filesystem::rename(backup, preferences);
        // Only a matching manual success may retire its previously owned error.
        const auto repairedRequest = ++app.model.settingsSaveRequest;
        app.action({"save", {}, {}, {}, failedRequest}); // Obsolete success cannot clear it.
        app.action({"save", {}, {}, {}});                // Neither can unrelated id-zero actions.
        assert(app.save()); // Direct persistence cannot publish or retire a manual result.
        app.model.actions.clear();
        app.lastSave_ = 0;
        app.tick(app.model.now + 31); // Exercise actual automatic persistence.
        assert(app.model.settingsSaveResult.requestId == failedRequest &&
               !app.model.settingsSaveResult.success &&
               app.model.settingsSaveResult.error == saveError && app.model.status == saveError);
        app.action({"save", {}, {}, {}, repairedRequest});
        assert(app.model.settingsSaveResult.requestId == repairedRequest &&
               app.model.settingsSaveResult.success && app.model.settingsSaveResult.error.empty());
        assert(app.model.status.empty());

        // A repaired save must not erase newer feedback from another producer.
        std::filesystem::rename(preferences, backup);
        std::filesystem::create_directory(preferences);
        const auto secondFailure = ++app.model.settingsSaveRequest;
        app.action({"save", {}, {}, {}, secondFailure});
        assert(app.model.settingsSaveResult.requestId == secondFailure &&
               !app.model.settingsSaveResult.success &&
               !app.model.settingsSaveResult.error.empty());
        std::filesystem::remove(preferences);
        std::filesystem::rename(backup, preferences);
        Event newerFeedback;
        newerFeedback.type = "error";
        newerFeedback.server = connectionName;
        newerFeedback.text = "A newer transport error";
        app.reduce(newerFeedback);
        assert(app.model.status == newerFeedback.text);
        const auto latestSuccess = ++app.model.settingsSaveRequest;
        app.action({"save", {}, {}, {}, latestSuccess});
        assert(app.model.settingsSaveResult.requestId == latestSuccess &&
               app.model.settingsSaveResult.success && app.model.settingsSaveResult.error.empty());
        assert(app.model.status == newerFeedback.text);

        // Live invalid profile edits cannot escape through direct save or autosave.
        app.model.firstRun = false;
        assert(app.save());
        const auto profilePath = preferences.parent_path() / "config.toml";
        auto profileBytes = [&] {
            std::ifstream input(profilePath);
            return string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        };
        const auto validProfiles = profileBytes();
        const auto validNick = app.model.identities.front().nick;
        app.model.identities.front().nick = "#bad";
        const auto invalidRequest = ++app.model.settingsSaveRequest;
        app.action({"save", {}, {}, {}, invalidRequest});
        assert(app.model.settingsSaveResult.requestId == invalidRequest &&
               !app.model.settingsSaveResult.success &&
               !app.model.settingsSaveResult.error.empty());
        const auto invalidError = app.model.settingsSaveResult.error;
        assert(profileBytes() == validProfiles);
        assert(!app.save() && profileBytes() == validProfiles);
        app.tick(app.model.now + 31);
        assert(profileBytes() == validProfiles &&
               app.model.settingsSaveResult.requestId == invalidRequest &&
               app.model.settingsSaveResult.error == invalidError);
        app.model.identities.front().nick = validNick;
        assert(app.save());

        // Shutdown's final core tick must materialize accepted commands before persistence.
        auto &utility = app.buffer(app.model.servers.front().name, "*server*", "server");
        app.model.activeBuffer = utility.id;
        const string shutdownQuery =
            utility.server + "/" + app.fold(utility.server, "ShutdownPeer");
        app.model.actions.push_back({"command", utility.id, "/query ShutdownPeer", {}});
        app.tick(app.model.now + .1);
        assert(app.model.actions.empty() && app.model.activeBuffer == shutdownQuery);
        assert(app.model.buffers.at(shutdownQuery).kind == "query");
        assert(app.save());
        Model shutdownReload;
        app.storage_.load(shutdownReload);
        assert(shutdownReload.activeBuffer == shutdownQuery);
        assert(shutdownReload.buffers.at(shutdownQuery).target == "ShutdownPeer");
    }
};
} // namespace blast
int main() {
    const auto path = std::filesystem::temp_directory_path() /
                      ("blastirc-reducer-check-" + std::to_string(getpid()) + "-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path);
    const char *config = std::getenv("XDG_CONFIG_HOME");
    const char *data = std::getenv("XDG_DATA_HOME");
    const std::string savedConfig = config ? config : "", savedData = data ? data : "";
    const bool hadConfig = config, hadData = data;
    setenv("XDG_CONFIG_HOME", path.c_str(), 1);
    setenv("XDG_DATA_HOME", path.c_str(), 1);
    if (std::getenv("BLAST_TEST_SEARCH"))
        blast::ApplicationRegression::privateSojuSearch();
    else
        blast::ApplicationRegression::run();
    if (hadConfig)
        setenv("XDG_CONFIG_HOME", savedConfig.c_str(), 1);
    else
        unsetenv("XDG_CONFIG_HOME");
    if (hadData)
        setenv("XDG_DATA_HOME", savedData.c_str(), 1);
    else
        unsetenv("XDG_DATA_HOME");
    std::filesystem::remove_all(path);
    std::cout << "application reducer checks passed\n";
}
