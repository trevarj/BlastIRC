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
    static void repeatSearch() {
        Application app;
        app.model = Model{};
        app.savingAllowed_ = false;
        auto &first = app.buffer("repeat-one", "#source");
        auto &second = app.buffer("repeat-two", "#source");
        Message message;
        message.id = "same-message-id";
        message.text = "first alpha beta";
        app.insert(first, message, false);
        message.text = "second alpha beta";
        app.insert(second, message, false);
        const string firstId = first.id, secondId = second.id;
        app.action({"search", firstId, "alpha", {}});
        auto &firstResults = app.buffer(first.server, "*search*", "search");
        assert(firstResults.searchSource == firstId && firstResults.messages.size() == 1);
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
        bool sourceHistoryEnded = false;
        size_t searchEnds = 0;
        const auto drain = [&] {
            vector<Event> incoming;
            for (auto &[name, connected] : app.clients_)
                for (auto &event : connected->poll())
                    incoming.push_back(std::move(event));
            for (auto &event : incoming) {
                if (event.server == childName && event.type == "history_end" &&
                    event.tags["direction"] != "search")
                    sourceHistoryEnded = true;
                if (event.server == childName && event.type == "history_end" &&
                    event.tags["direction"] == "search")
                    ++searchEnds;
                if (event.type == "history_begin" || event.type == "history_end" ||
                    event.tags.contains("search") || event.type == "error")
                    std::cout << "EVENT " << event.type << " target=" << event.target
                              << " text=" << event.text << " label=" << event.label
                              << " direction=" << event.tags["direction"] << '\n';
                app.reduce(std::move(event));
            }
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
        wait([&] { return sourceHistoryEnded && !source.loading; },
             "JOIN-triggered initial history did not complete before remote-only marker");
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
        app.action({"search", source.id, marker, {}});
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

        peer.send(":server 332 Me #target :current live topic");
        peer.send(":server BATCH +topic-history chathistory #target");
        peer.send("@batch=topic-history :Alice!user@host TOPIC #target :historic topic change");
        peer.send("@batch=topic-history :server 332 Me #target :historic snapshot");
        peer.send("@batch=topic-history :Bob!user@host PRIVMSG #target "
                  ":Alice changed the topic: arbitrary historical chat");
        peer.send("@batch=topic-history :server 331 Me #target :No historic topic");
        peer.send(":server BATCH -topic-history");
        drain();
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
        app.command(app.model.buffers.at(origin), "/search #target words");
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
        app.reduce(history);
        assert(room.messages.size() == 3 && room.messages[1].id == historyLocalId &&
               room.messages[1].sendStatus == SendStatus::Queued &&
               room.messages.back().sendStatus == SendStatus::Confirmed);
        history.id = "matched-history-status";
        history.label = admission.label;
        app.reduce(history);
        assert(room.messages.size() == 3 && room.messages[1].id == history.id &&
               room.messages[1].sendStatus == SendStatus::Confirmed);
        sent.label = admission.label;
        app.reduce(sent);
        assert(room.messages[1].sendStatus == SendStatus::Confirmed);
        admission.label = "history-status-id";
        app.reduce(admission);
        history.id = room.messages.back().id;
        history.label = "history-request-label";
        app.reduce(history);
        assert(room.messages.size() == 4 && room.messages.back().id == history.id &&
               room.messages.back().label == admission.label &&
               room.messages.back().sendStatus == SendStatus::Confirmed);

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
        app.reduce(echo);
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
        app.reduce(echo);
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
        assert(room.messages.back().own);
        event.nick = "Forced";
        event.id = "hidden-forced";
        event.tags["force-presence"] = "1";
        app.reduce(event);
        expectRow(event, ActivityKind::Join, "Forced joined #activity");
        assert(room.messages.size() == beforeHidden + 2 && app.model.effects.size() == 2);

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
    static void run() {
        commandArgumentContext();
        repeatSearch();
        appOriginAfterSwitch();
        localOriginErrors();
        closedQueuedCommandOrigin();
        networkStatusErrors();
        sojuHierarchy();
        globalRead();
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
        assert(app.historyCursor(cached, "after=" + historyMessage.id) ==
               "after=timestamp=2026-11-04T12:00:00.123Z");
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
        app.requestHistory(zncRoom);
        assert(!zncRoom.loading); // A configured bouncer alone does not promise the module.
        Event nativeCaps;
        nativeCaps.type = "caps";
        nativeCaps.server = znc.name;
        nativeCaps.params = {"znc.in/playback"};
        app.model.networks[znc.name].state = "Ready";
        zncRoom.joined = true;
        app.reduce(nativeCaps);
        assert(app.historyCursor(zncRoom, historyMessage.id) == "timestamp=" + historyMessage.time);
        app.requestHistory(zncRoom);
        assert(zncRoom.loading);
        assert(app.gaps_.at(zncRoom.id).cursor == "timestamp=" + historyMessage.time);
        app.clients_.erase(znc.name); // This admission check never starts a transport.

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
        assert(app.model.settings.ircChannels.empty());
        app.command(room, "/messagestyle irc");
        assert(app.model.settings.ircChannels == set<string>{room.id});
        Model preferenceReload;
        app.storage_.load(preferenceReload);
        assert(preferenceReload.settings.ircChannels == app.model.settings.ircChannels);
        app.command(room, "/messagestyle bubbles");
        assert(app.model.settings.ircChannels.empty());
        app.command(room, "/messagestyle");
        assert(app.model.settings.ircChannels == set<string>{room.id});
        app.command(room, "/messagestyle invalid");
        assert(app.model.settings.ircChannels == set<string>{room.id});
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
        app.model.settings.ircChannels.insert(sourceId);
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
        assert(app.model.settings.ircChannels.contains(destinationId) &&
               !app.model.settings.ircChannels.contains(sourceId) &&
               app.model.settings.ircChannels.contains(room.id));
        assert(app.model.settings.workspace.at("last_buffer:rename") == destinationId &&
               app.model.settings.workspace.at("settings_feedback") == sourceId);
        assert(app.model.settings.workspace.at("visible_buffers") == destinationId);
        assert(app.save());
        app.storage_.load(preferenceReload);
        assert(preferenceReload.settings.ircChannels == app.model.settings.ircChannels);
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
        assert(app.model.settings.ircChannels.contains(closingId));
        assert(app.save());
        app.storage_.load(preferenceReload);
        assert(preferenceReload.settings.ircChannels.contains(closingId) &&
               !preferenceReload.buffers.contains(closingId));
        auto &reopened = app.buffer("test", closingTarget, "query");
        assert(reopened.id == closingId && app.model.settings.ircChannels.contains(reopened.id));
        app.command(reopened, "/messagestyle bubbles");
        app.storage_.load(preferenceReload);
        assert(!preferenceReload.settings.ircChannels.contains(closingId) &&
               preferenceReload.settings.ircChannels.contains(destinationId));

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
