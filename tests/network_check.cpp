#include "avatar.hpp"
#include "irc/network.hpp"
#include "irc/protocol.hpp"
#include "irc/sts.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <optional>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace blast;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {
void require(bool condition, const char *reason) {
    if (!condition)
        throw std::runtime_error(reason);
}
struct TemporaryDirectory {
    fs::path path;
    TemporaryDirectory() {
        string name = (fs::temp_directory_path() / "blastirc-network-check-XXXXXX").string();
        const auto *created = ::mkdtemp(name.data());
        require(created != nullptr, "Cannot create isolated network-check directory");
        path = created;
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};
struct Environment {
    const char *name;
    std::optional<string> previous;
    Environment(const char *key, const fs::path &value) : name(key) {
        if (const auto *old = std::getenv(name))
            previous = old;
        require(::setenv(name, value.c_str(), 1) == 0, "Cannot isolate network-check environment");
    }
    ~Environment() {
        if (previous)
            ::setenv(name, previous->c_str(), 1);
        else
            ::unsetenv(name);
    }
};
using TlsContext = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>;
TlsContext trustedLocalhost(const fs::path &certificateFile) {
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> generator(
        EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr), EVP_PKEY_CTX_free);
    require(generator && EVP_PKEY_keygen_init(generator.get()) == 1 &&
                EVP_PKEY_CTX_set_ec_paramgen_curve_nid(generator.get(), NID_X9_62_prime256v1) == 1,
            "Cannot initialize temporary TLS key");
    EVP_PKEY *generated = nullptr;
    require(EVP_PKEY_keygen(generator.get(), &generated) == 1, "Cannot generate temporary TLS key");
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(generated, EVP_PKEY_free);
    std::unique_ptr<X509, decltype(&X509_free)> certificate(X509_new(), X509_free);
    require(certificate && X509_set_version(certificate.get(), 2) == 1 &&
                ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) == 1 &&
                X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60) &&
                X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 3600) &&
                X509_set_pubkey(certificate.get(), key.get()) == 1,
            "Cannot initialize temporary TLS certificate");
    auto *subject = X509_get_subject_name(certificate.get());
    require(X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                                       reinterpret_cast<const unsigned char *>("localhost"), -1, -1,
                                       0) == 1 &&
                X509_set_issuer_name(certificate.get(), subject) == 1,
            "Cannot name temporary TLS certificate");
    X509V3_CTX extensionContext{};
    X509V3_set_ctx(&extensionContext, certificate.get(), certificate.get(), nullptr, nullptr, 0);
    auto extension = [&](int nid, const char *value) {
        std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> item(
            X509V3_EXT_conf_nid(nullptr, &extensionContext, nid, value), X509_EXTENSION_free);
        require(item && X509_add_ext(certificate.get(), item.get(), -1) == 1,
                "Cannot extend temporary TLS certificate");
    };
    extension(NID_basic_constraints, "critical,CA:TRUE");
    extension(NID_key_usage, "critical,digitalSignature,keyCertSign");
    extension(NID_ext_key_usage, "serverAuth");
    extension(NID_subject_alt_name, "DNS:localhost,IP:127.0.0.1");
    require(X509_sign(certificate.get(), key.get(), EVP_sha256()) > 0,
            "Cannot sign temporary TLS certificate");
    std::unique_ptr<BIO, decltype(&BIO_free)> file(BIO_new_file(certificateFile.c_str(), "w"),
                                                   BIO_free);
    require(file && PEM_write_bio_X509(file.get(), certificate.get()) == 1 &&
                BIO_flush(file.get()) == 1,
            "Cannot write temporary TLS trust anchor");
    TlsContext context(SSL_CTX_new(TLS_server_method()), SSL_CTX_free);
    require(context && SSL_CTX_set_min_proto_version(context.get(), TLS1_2_VERSION) == 1 &&
                SSL_CTX_use_certificate(context.get(), certificate.get()) == 1 &&
                SSL_CTX_use_PrivateKey(context.get(), key.get()) == 1 &&
                SSL_CTX_check_private_key(context.get()) == 1,
            "Cannot initialize loopback TLS server");
    return context;
}
struct Fd {
    int value = -1;
    ~Fd() { reset(); }
    void reset(int next = -1) {
        if (value >= 0)
            ::close(value);
        value = next;
    }
};
void waitFd(int fd, short flags, Clock::time_point deadline) {
    for (;;) {
        require(Clock::now() < deadline, "Loopback socket/TLS deadline expired");
        const auto remaining =
            std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
        pollfd item{fd, flags, 0};
        const int result = ::poll(&item, 1, std::max(1, static_cast<int>(remaining.count())));
        if (result < 0 && errno == EINTR)
            continue;
        require(result > 0 && !(item.revents & POLLNVAL), "Loopback socket/TLS wait failed");
        return;
    }
}
// One nonblocking peer drives the real worker; no test-side server thread.
struct WirePeer {
    Fd listener, connection;
    SSL_CTX *context;
    std::unique_ptr<SSL, decltype(&SSL_free)> tls{nullptr, SSL_free};
    string buffered;
    int port = 0;
    explicit WirePeer(SSL_CTX *ctx) : context(ctx) {
        listener.value = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        require(listener.value >= 0, "Cannot create loopback listener");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(::bind(listener.value, reinterpret_cast<sockaddr *>(&address), sizeof(address)) ==
                        0 &&
                    ::listen(listener.value, 1) == 0,
                "Cannot bind loopback listener");
        socklen_t size = sizeof(address);
        require(::getsockname(listener.value, reinterpret_cast<sockaddr *>(&address), &size) == 0,
                "Cannot read loopback port");
        port = ntohs(address.sin_port);
    }
    void tlsWait(int result, Clock::time_point deadline, const char *operation) {
        const int systemError = errno;
        const int error = SSL_get_error(tls.get(), result);
        if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) {
            std::ostringstream message;
            message << "Loopback TLS " << operation << " failed: SSL_get_error=" << error
                    << "; state=" << SSL_state_string_long(tls.get());
            if (error == SSL_ERROR_SYSCALL)
                message << "; errno=" << systemError << " (" << std::strerror(systemError) << ')';
            message << "; error_queue=[";
            bool first = true;
            for (unsigned long detail; (detail = ERR_get_error()) != 0;) {
                char description[256];
                ERR_error_string_n(detail, description, sizeof(description));
                message << (first ? "" : "; ") << description;
                first = false;
            }
            message << ']'; // Error codes only, never TLS/IRC payloads or error-data strings.
            // Capture trust/temp state before scoped fixture teardown restores
            // the environment and deletes the private CA. Never print path values.
            for (const char *key : {"SSL_CERT_FILE", "NIX_SSL_CERT_FILE", "SSL_CERT_DIR", "TMPDIR",
                                    "TMP", "TEMP", "NIX_BUILD_TOP"}) {
                const char *value = std::getenv(key);
                message << "; " << key << "={set=" << (value != nullptr);
                if (value) {
                    std::error_code fileError, parentError;
                    const fs::path path(value);
                    message << ",exists=" << fs::exists(path, fileError)
                            << ",status-error=" << fileError.value()
                            << ",parent-exists=" << fs::exists(path.parent_path(), parentError)
                            << ",parent-error=" << parentError.value();
                    if (std::strcmp(key, "SSL_CERT_FILE") == 0 ||
                        std::strcmp(key, "NIX_SSL_CERT_FILE") == 0) {
                        std::unique_ptr<BIO, decltype(&BIO_free)> file(BIO_new_file(value, "r"),
                                                                       BIO_free);
                        std::unique_ptr<X509, decltype(&X509_free)> certificate(
                            file ? PEM_read_bio_X509(file.get(), nullptr, nullptr, nullptr)
                                 : nullptr,
                            X509_free);
                        message << ",certificate-readable=" << bool(certificate)
                                << ",matches-fixture-certificate="
                                << (certificate &&
                                    X509_cmp(certificate.get(),
                                             SSL_CTX_get0_certificate(context)) == 0);
                        ERR_clear_error();
                    }
                }
                message << '}';
            }
            throw std::runtime_error(message.str());
        }
        waitFd(connection.value, error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, deadline);
    }
    void accept() {
        tls.reset();
        connection.reset();
        buffered.clear();
        const auto deadline = Clock::now() + 3s;
        while (connection.value < 0) {
            waitFd(listener.value, POLLIN, deadline);
            connection.value =
                ::accept4(listener.value, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            require(connection.value >= 0 || errno == EINTR || errno == EAGAIN ||
                        errno == EWOULDBLOCK,
                    "Cannot accept production client");
        }
        tls.reset(SSL_new(context));
        require(tls && SSL_set_fd(tls.get(), connection.value) == 1,
                "Cannot attach loopback TLS connection");
        for (;;) {
            require(Clock::now() < deadline, "Loopback TLS handshake deadline expired");
            const int result = SSL_accept(tls.get());
            if (result == 1)
                return;
            tlsWait(result, deadline, "accept");
        }
    }
    void send(const string &line) {
        const string frame = line + "\r\n";
        const auto deadline = Clock::now() + 3s;
        size_t offset = 0;
        while (offset < frame.size()) {
            require(Clock::now() < deadline, "Loopback TLS send deadline expired");
            const int result = SSL_write(tls.get(), frame.data() + offset,
                                         static_cast<int>(frame.size() - offset));
            if (result > 0)
                offset += static_cast<size_t>(result);
            else
                tlsWait(result, deadline, "write");
        }
    }
    irc::WireMessage read(Clock::time_point deadline = Clock::now() + 3s) {
        for (;;) {
            require(Clock::now() < deadline, "Production wire-frame deadline expired");
            const auto end = buffered.find("\r\n");
            if (end != string::npos) {
                auto message =
                    irc::parse(std::string_view(buffered).substr(0, end), irc::incomingCoreLimit);
                require(message.has_value(), "Production emitted an invalid IRC frame");
                buffered.erase(0, end + 2);
                return std::move(*message);
            }
            char bytes[4096];
            const int result = SSL_read(tls.get(), bytes, sizeof(bytes));
            if (result > 0) {
                buffered.append(bytes, static_cast<size_t>(result));
                require(buffered.size() < 65536, "Production wire frame exceeds test bound");
            } else
                tlsWait(result, deadline, "read");
        }
    }
    irc::WireMessage expect(const string &command, const vector<string> &params) {
        auto message = read();
        require(message.command == command && message.params == params,
                "Unexpected production wire command/parameters");
        return message;
    }
    void fence() {
        send("PING :network-check-fence");
        expect("PONG", {"network-check-fence"});
    }
    // Clamp the real client's socket without adding a production test hook.
    // This fixture already requires Linux; /proc keeps the hook test-side.
    int limitClientWrites() {
        sockaddr_in address{};
        socklen_t size = sizeof(address);
        require(::getsockname(connection.value, reinterpret_cast<sockaddr *>(&address), &size) == 0,
                "Cannot identify loopback peer address");
        for (const auto &entry : fs::directory_iterator("/proc/self/fd")) {
            const int fd = std::stoi(entry.path().filename().string());
            sockaddr_in remote{};
            size = sizeof(remote);
            if (::getpeername(fd, reinterpret_cast<sockaddr *>(&remote), &size) != 0 ||
                remote.sin_family != AF_INET || remote.sin_port != address.sin_port ||
                remote.sin_addr.s_addr != address.sin_addr.s_addr)
                continue;
            const int sendBytes = 4096, receiveBytes = 256 * 1024;
            // Keep the receive window above loopback TCP segments to avoid slow
            // tiny-window recovery; even Linux's doubled buffer cannot hold 1.6 MiB.
            require(::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sendBytes, sizeof(sendBytes)) == 0 &&
                        ::setsockopt(connection.value, SOL_SOCKET, SO_RCVBUF, &receiveBytes,
                                     sizeof(receiveBytes)) == 0,
                    "Cannot bound loopback write buffering");
            return fd;
        }
        throw std::runtime_error("Cannot find production loopback socket");
    }
    void quietUntil(Clock::time_point deadline) {
        require(buffered.empty() && SSL_pending(tls.get()) == 0,
                "Unexpected queued production wire frame");
        while (Clock::now() < deadline) {
            const auto remaining =
                std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
            pollfd item{connection.value, POLLIN, 0};
            const int result = ::poll(&item, 1, std::max(1, static_cast<int>(remaining.count())));
            if (result < 0 && errno == EINTR)
                continue;
            require(result == 0, "Production emitted a wire frame during a quiet interval");
        }
    }
};
std::string_view tag(const Event &event, const string &name) {
    const auto found = event.tags.find(name);
    return found == event.tags.end() ? std::string_view{} : found->second;
}
void collect(NetworkClient &client, vector<Event> &events) {
    for (auto &event : client.poll()) {
        if (event.type == "state" && event.text == "Failed")
            throw std::runtime_error("Production client entered Failed: " +
                                     string(tag(event, "error")));
        events.push_back(std::move(event));
    }
}
template <class Predicate>
size_t waitEvent(NetworkClient &client, vector<Event> &events, size_t from, Predicate matches) {
    const auto deadline = Clock::now() + 3s;
    for (;;) {
        collect(client, events);
        for (; from < events.size(); ++from)
            if (matches(events[from]))
                return from;
        require(Clock::now() < deadline, "Production event deadline expired");
        std::this_thread::sleep_for(1ms);
    }
}
ServerConfig server(int port, const string &sasl = "", const string &bouncer = "none") {
    ServerConfig result;
    result.name = "wire-check";
    result.host = "127.0.0.1";
    result.port = port;
    result.tls = true;
    result.sasl = sasl;
    result.bouncer = bouncer;
    return result;
}
set<string> capNames(const string &list) {
    set<string> names;
    std::istringstream words(list);
    for (string word; words >> word;)
        names.insert(word.substr(0, word.find('=')));
    return names;
}
void registerClient(WirePeer &peer, NetworkClient &client, vector<Event> &events,
                    const Identity &identity, const string &caps, bool sasl = false,
                    bool shortSts = false, const std::function<void()> &beforeWelcome = {},
                    bool subscribeAvatar = true, const string &binding = "", bool catalog = false,
                    const string &rejectBinding = "") {
    client.start();
    try {
        peer.accept();
    } catch (const std::exception &error) {
        std::ostringstream message;
        message << error.what() << "; client_states=[";
        for (const auto &event : client.poll())
            if (event.type == "state")
                message << event.text << " (" << tag(event, "error") << "); ";
        message << ']';
        throw std::runtime_error(message.str());
    }
    peer.expect("CAP", {"LS", "302"});
    if (shortSts) {
        // STS rounds to whole seconds: advertise just after an observed boundary
        // so the one-second policy cannot disappear during the SASL exchange.
        const auto deadline = Clock::now() + 3s;
        const auto second =
            std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        while (std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()) ==
               second) {
            require(Clock::now() < deadline, "STS clock-boundary deadline expired");
            std::this_thread::sleep_for(1ms);
        }
    }
    peer.send(":server CAP * LS :" + caps);
    const auto offered = capNames(caps);
    set<string> requested;
    bool nick = false, user = false, mechanism = false, authenticated = false;
    bool bound = false;
    const auto deadline = Clock::now() + 3s;
    for (;;) {
        const auto message = peer.read(deadline);
        if (message.command == "NICK") {
            require(!nick && message.params == vector<string>{identity.nick},
                    "Incorrect registration NICK");
            nick = true;
        } else if (message.command == "USER") {
            require(!user && message.params ==
                                 vector<string>{identity.username, "0", "*", identity.realname},
                    "Incorrect registration USER");
            user = true;
        } else if (message.command == "CAP" && message.params.size() == 2 &&
                   message.params[0] == "REQ") {
            const auto names = capNames(message.params[1]);
            require(!names.empty() && !names.contains("sts"), "STS must never appear in CAP REQ");
            for (const auto &name : names)
                require(offered.contains(name) && requested.insert(name).second,
                        "Production requested an unsupported or duplicate capability");
            peer.send(":server CAP " + identity.nick + " ACK :" + message.params[1]);
        } else if (message.command == "AUTHENTICATE") {
            require(sasl && message.params.size() == 1, "Unexpected SASL command");
            if (!mechanism) {
                require(message.params[0] == "PLAIN", "Production did not select SASL PLAIN");
                mechanism = true;
                peer.send("AUTHENTICATE +");
            } else {
                require(!authenticated, "Unexpected extra SASL response");
                const string plain =
                    identity.username + '\0' + identity.username + '\0' + identity.password;
                string encoded(4 * ((plain.size() + 2) / 3) + 1, '\0');
                const int length =
                    EVP_EncodeBlock(reinterpret_cast<unsigned char *>(encoded.data()),
                                    reinterpret_cast<const unsigned char *>(plain.data()),
                                    static_cast<int>(plain.size()));
                require(length > 0, "Cannot encode expected SASL response");
                encoded.resize(static_cast<size_t>(length));
                require(message.params[0] == encoded, "Incorrect production SASL PLAIN response");
                authenticated = true;
                peer.send(":server 903 " + identity.nick + " :SASL authentication successful");
            }
        } else if (message.command == "BOUNCER") {
            require(!binding.empty() && !bound && authenticated == sasl &&
                        message.params == vector<string>{"BIND", binding},
                    "BOUNCER BIND must follow SASL and precede CAP END");
            bound = true;
        } else {
            require(message.command == "CAP" && message.params == vector<string>{"END"} && nick &&
                        user && authenticated == sasl,
                    "Registration ended before CAP/SASL completed");
            break;
        }
    }
    auto expected = offered;
    expected.erase("sts");
    require(requested == expected, "Production omitted an offered test capability");
    require(bound == !binding.empty(), "Configured BOUNCER binding was omitted");
    if (beforeWelcome)
        beforeWelcome();
    if (!rejectBinding.empty()) {
        require(!binding.empty(), "Binding rejection fixture needs a bound client");
        // One TLS write places FAIL and a misleading welcome in the same read.
        peer.send(":server FAIL BOUNCER " + rejectBinding + " BIND" +
                  (rejectBinding == "INVALID_NETID" ? " " + binding : "") +
                  " :wire-secret\r\n:server 001 " + identity.nick + " :Welcome");
        const auto failedDeadline = Clock::now() + 3s;
        bool failed = false;
        while (!failed) {
            for (auto &event : client.poll()) {
                require(event.type != "state" || event.text != "Ready",
                        "Rejected BIND must never become Ready, even with same-read 001");
                if (event.type == "state" && event.text == "Failed") {
                    failed = true;
                    const auto error = tag(event, "error");
                    require(!error.empty() && error.find("wire-secret") == string::npos,
                            "BIND failure must expose a safe fatal connection error");
                    if (rejectBinding == "ACCOUNT_REQUIRED")
                        require(error.find("ACCOUNT_REQUIRED") != string::npos,
                                "BIND failure must preserve the safe authentication error code");
                }
                events.push_back(std::move(event));
            }
            require(Clock::now() < failedDeadline, "Rejected BIND did not terminate registration");
            if (!failed)
                std::this_thread::sleep_for(1ms);
        }
        client.stop();
        for (auto &event : client.poll()) {
            require(event.type != "state" || (event.text != "Ready" && event.text != "Connecting"),
                    "Rejected BIND must not retry or consume a later welcome");
            events.push_back(std::move(event));
        }
        return;
    }
    peer.send(":server 001 " + identity.nick + " :Welcome");
    waitEvent(client, events, 0, [&](const Event &event) {
        return event.type == "state" && event.text == "Ready" && event.nick == identity.nick;
    });
    if (catalog)
        peer.expect("BOUNCER", {"LISTNETWORKS"});
    if (expected.contains("draft/metadata-2") && subscribeAvatar)
        peer.expect("METADATA", {"*", "SUB", "avatar"});
    // Any extra/deferred CAP request is a wire failure, not silently discarded.
    peer.fence();
    const auto capsEvent = std::find_if(events.rbegin(), events.rend(),
                                        [](const Event &event) { return event.type == "caps"; });
    require(capsEvent != events.rend() &&
                set<string>(capsEvent->params.begin(), capsEvent->params.end()) == expected,
            "Ready capabilities do not match acknowledged test capabilities");
}
void registrationValidationChecks(SSL_CTX *context) {
    Identity invalid;
    invalid.nick = "#bad";
    NetworkClient rejected(server(1), invalid);
    rejected.start();
    const auto deadline = Clock::now() + 3s;
    bool failed = false;
    while (!failed) {
        for (const auto &event : rejected.poll()) {
            require(event.type != "state" || event.text != "Connecting",
                    "An invalid nickname reached transport connection");
            if (event.type == "state" && event.text == "Failed") {
                require(tag(event, "error") == "Invalid IRC connection settings",
                        "Invalid nickname failed for an unrelated reason");
                failed = true;
            }
        }
        require(Clock::now() < deadline, "Invalid nickname validation deadline expired");
        if (!failed)
            std::this_thread::sleep_for(1ms);
    }
    rejected.stop();

    WirePeer peer(context);
    Identity unicode;
    unicode.nick = "猫🙂";
    unicode.username = "用户";
    unicode.realname = "Unicode real name with spaces";
    NetworkClient accepted(server(peer.port), unicode);
    vector<Event> events;
    registerClient(peer, accepted, events, unicode, "message-tags");
    accepted.stop();
    collect(accepted, events);
}

int64_t wallSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
void stsChecks(SSL_CTX *context, const Identity &identity) {
    WirePeer peer(context);
    {
        NetworkClient client(server(peer.port, "PLAIN"), identity);
        vector<Event> events;
        registerClient(
            peer, client, events, identity,
            "sasl=PLAIN sts=duration=1,port=bogus message-tags echo-message batch server-time",
            true, true);
        const auto policy = irc::loadSts("127.0.0.1");
        require(policy && policy->port == peer.port && policy->expires > wallSeconds(),
                "Verified TLS STS did not retain the actual TLS port");
        const auto deadline = Clock::now() + 3s;
        while (wallSeconds() < policy->expires) {
            require(Clock::now() < deadline, "Short STS policy did not reach observed expiry");
            std::this_thread::sleep_for(1ms);
        }
        require(!irc::loadSts("127.0.0.1"), "Expired STS policy remained active");
        peer.send(":server CAP " + identity.nick + " DEL :sts");
        peer.fence();
        client.stop();
        collect(client, events);
        const auto renewed = irc::loadSts("127.0.0.1");
        require(renewed && renewed->port == peer.port && renewed->expires > wallSeconds() &&
                    renewed->expires > policy->expires,
                "Disconnect did not renew expired STS after CAP DEL");
    }
    {
        // Reuse the endpoint: the preceding persistent policy must still select it.
        NetworkClient client(server(peer.port, "PLAIN"), identity);
        vector<Event> events;
        registerClient(peer, client, events, identity,
                       "sasl=PLAIN sts=duration=60,port=bogus message-tags", true);
        require(irc::loadSts("127.0.0.1").has_value(), "Positive STS policy was not stored");
        peer.send(":server CAP " + identity.nick + " NEW :sts=duration=0,port=bogus");
        peer.fence();
        require(!irc::loadSts("127.0.0.1"), "CAP NEW duration=0 did not remove STS immediately");
        client.stop();
        collect(client, events);
        require(!irc::loadSts("127.0.0.1"), "Disconnect resurrected a revoked STS policy");
    }
}
void clientTagPolicyChecks(SSL_CTX *context, const Identity &identity) {
    WirePeer peer(context);
    NetworkClient client(server(peer.port), identity);
    vector<Event> events;
    registerClient(peer, client, events, identity, "message-tags");
    const auto policy = [&](const string &value) {
        peer.send(":server 005 " + identity.nick + " CLIENTTAGDENY=" + value + " :supported");
        peer.fence();
    };
    const auto marker = [&] {
        client.send("#policy", "policy-marker");
        peer.expect("PRIVMSG", {"#policy", "policy-marker"});
    };
    // A persisted own reaction need not exist in this fresh network session.
    // Removal intent, not a session toggle cache, selects permission and wire tag.
    policy("*,-draft/unreact,-reply");
    client.react("#policy", "persisted-reaction", ":)", true);
    const auto persistedRemoval = peer.expect("TAGMSG", {"#policy"});
    require(persistedRemoval.tags.at("+draft/unreact") == ":)" &&
                persistedRemoval.tags.at("+reply") == "persisted-reaction" &&
                !persistedRemoval.tags.contains("+draft/react"),
            "Fresh-session removal lost persisted reaction intent");
    client.react("#policy", "persisted-reaction", ":)", false);
    marker();
    policy("*,-draft/react,-reply");
    client.react("#policy", "persisted-reaction", ":)", true);
    marker();
    for (const auto *deny : {"draft/react", "draft/*", "*"}) {
        policy(deny);
        client.react("#policy", "blocked-reaction", ":)", false);
        // The marker is queued after react: any forbidden TAGMSG fails expect.
        marker();
    }
    policy("*,-draft/*");
    client.react("#policy", "blocked-reply", ":)", false);
    client.send("#policy", "reply-denied", "reference", "", "alice");
    const auto deniedReply = peer.expect("PRIVMSG", {"#policy", "alice: reply-denied"});
    require(!deniedReply.tags.contains("+reply"), "CLIENTTAGDENY reply denial leaked a wire tag");
    policy("*yp*");
    client.typing("#policy", "active");
    marker();
    policy("*,-draft/*,-reply");
    client.react("#policy", "allowed-reaction", ":)", false);
    const auto added = peer.expect("TAGMSG", {"#policy"});
    require(added.tags.at("+reply") == "allowed-reaction" && added.tags.at("+draft/react") == ":)",
            "Wildcard exception did not permit valid reaction tags");
    client.react("#policy", "allowed-reaction", ":)", true);
    const auto removed = peer.expect("TAGMSG", {"#policy"});
    require(removed.tags.at("+reply") == "allowed-reaction" &&
                removed.tags.at("+draft/unreact") == ":)",
            "Wildcard exception did not permit valid reaction removal tags");
    client.send("#policy", "reply-allowed", "reference", "", "alice");
    require(peer.expect("PRIVMSG", {"#policy", "reply-allowed"}).tags.at("+reply") == "reference",
            "CLIENTTAGDENY exception did not permit a reply");
    policy("*");
    peer.send(":server 005 " + identity.nick + " -CLIENTTAGDENY :supported");
    peer.fence();
    client.send("#policy", "policy-removed", "reference");
    require(peer.expect("PRIVMSG", {"#policy", "policy-removed"}).tags.at("+reply") == "reference",
            "Removed CLIENTTAGDENY left stale denial state");
    peer.send(":server CAP " + identity.nick + " DEL :message-tags");
    peer.fence();
    client.react("#policy", "unsupported-reaction", ":)", false);
    client.typing("#policy", "active");
    client.send("#policy", "unsupported-reply", "reference", "", "alice");
    require(peer.expect("PRIVMSG", {"#policy", "alice: unsupported-reply"}).tags.empty(),
            "Client-only tags escaped after message-tags was removed");
    client.stop();
}

void queuedReplyRevocationChecks(SSL_CTX *context, const Identity &identity) {
    set<string> previousThreads;
    for (const auto &entry : fs::directory_iterator("/proc/self/task"))
        previousThreads.insert(entry.path().filename().string());
    WirePeer peer(context);
    NetworkClient client(server(peer.port), identity);
    vector<Event> events;
    try {
        registerClient(peer, client, events, identity, "message-tags");
        peer.fence();
        collect(client, events);
        string worker;
        for (const auto &entry : fs::directory_iterator("/proc/self/task"))
            if (!previousThreads.contains(entry.path().filename().string())) {
                require(worker.empty(), "Cannot uniquely identify network worker");
                worker = entry.path().filename().string();
            }
        require(!worker.empty(), "Cannot identify network worker");
        const string gate = ":alice!u@h NOTICE #gate :gate";
        const auto from = events.size();
        peer.send(gate);
        const auto sampleIndex = waitEvent(client, events, from, [](const Event &event) {
            return event.type == "notice" && event.target == "#gate";
        });
        peer.fence();
        collect(client, events);
        const auto &sample = events[sampleIndex];
        // Match production eventSize to fill delivery without a timing race or hook.
        size_t bytes = sizeof(Event) + sample.server.capacity() + sample.target.capacity() +
                       sample.nick.capacity() + sample.text.capacity() + sample.id.capacity() +
                       sample.time.capacity() + sample.reply.capacity() + sample.label.capacity();
        for (const auto &[key, value] : sample.tags)
            bytes += key.capacity() + value.capacity() + sizeof(key) + sizeof(value) +
                     4 * sizeof(void *);
        bytes += sample.params.capacity() * sizeof(string);
        for (const auto &param : sample.params)
            bytes += param.capacity();
        const size_t slots = 16 * 1024 * 1024 / bytes;
        require(slots > 0 && slots < 32768, "Unexpected receive gate accounting");
        string fill;
        fill.reserve(slots * (gate.size() + 2));
        for (size_t i = 0; i < slots; ++i) {
            if (i)
                fill += "\r\n";
            fill += gate;
        }
        peer.send(fill);
        peer.fence(); // All slots admitted; one more identical event cannot fit.
        peer.send(gate + "\r\n:server CAP " + identity.nick + " DEL :message-tags");
        const auto deadline = Clock::now() + 3s;
        for (;;) {
            std::ifstream channel("/proc/self/task/" + worker + "/wchan");
            string state;
            channel >> state;
            require(!state.empty(), "Network worker wait channel unavailable");
            if (state.find("futex") != string::npos)
                break;
            require(Clock::now() < deadline, "Network worker delivery gate not observable (wait "
                                             "channel unavailable or not blocked)");
            std::this_thread::yield();
        }
        // Worker is inside receiveFrames, immediately before the same-record DEL.
        client.send("#reply", "queued raw", "remote-parent", "queued-reply", "alice");
        collect(client, events);
        const auto sent = peer.expect("PRIVMSG", {"#reply", "alice: queued raw"});
        require(!sent.tags.contains("+reply"), "Queued reply retained revoked tag permission");
        const auto pending = waitEvent(client, events, 0, [](const Event &event) {
            return event.label == "queued-reply" && tag(event, "pending") == "1";
        });
        require(tag(events[pending], "original-text") == "queued raw" &&
                    tag(events[pending], "submission") == "queued-reply",
                "Queued fallback lost raw recovery or submission identity");
        peer.send(":" + identity.nick + "!u@h PRIVMSG #reply :alice: queued raw");
        waitEvent(client, events, pending + 1, [](const Event &event) {
            return event.type == "message" && event.label == "queued-reply" &&
                   tag(event, "pending").empty();
        });
        peer.fence();
        collect(client, events);
        require(std::count_if(events.begin(), events.end(),
                              [](const Event &event) {
                                  return event.type == "sent" && event.label == "queued-reply" &&
                                         tag(event, "echo-expected") == "0";
                              }) == 1,
                "Queued fallback emitted duplicate send completion");
        client.send("#reply", "rejected raw", "", "noecho-rejected", "alice");
        peer.expect("PRIVMSG", {"#reply", "alice: rejected raw"});
        waitEvent(client, events, 0, [](const Event &event) {
            return event.type == "sent" && event.label == "noecho-rejected";
        });
        peer.send("@label=noecho-rejected :server FAIL PRIVMSG INVALID_TARGET :rejected");
        const auto rejected = waitEvent(client, events, 0, [](const Event &event) {
            return event.label == "noecho-rejected" && tag(event, "send-failed") == "1";
        });
        require(tag(events[rejected], "original-text") == "rejected raw",
                "Completed no-echo send lost explicit rejection recovery");
        client.send("#reply", "successful raw", "", "noecho-complete", "alice");
        peer.expect("PRIVMSG", {"#reply", "alice: successful raw"});
        waitEvent(client, events, 0, [](const Event &event) {
            return event.type == "sent" && event.label == "noecho-complete" &&
                   tag(event, "echo-expected") == "0";
        });
        client.stop();
        collect(client, events);
        require(std::none_of(events.begin(), events.end(),
                             [](const Event &event) {
                                 return event.label == "noecho-complete" &&
                                        tag(event, "send-failed") == "1";
                             }),
                "Shutdown recovered a completed no-echo send as a failure");
    } catch (const std::exception &error) {
        for (auto &event : client.poll())
            events.push_back(std::move(event));
        std::ostringstream diagnostic;
        diagnostic << error.what() << "; client-states=[";
        for (const auto &event : events)
            if (event.type == "state")
                diagnostic << event.text << " (" << tag(event, "error") << "); ";
        diagnostic << ']';
        throw std::runtime_error(diagnostic.str());
    }
}

void replyFallbackChecks(SSL_CTX *context, const Identity &identity) {
    WirePeer peer(context);
    NetworkClient client(server(peer.port), identity);
    vector<Event> events;
    registerClient(peer, client, events, identity,
                   "message-tags batch standard-replies labeled-response echo-message "
                   "draft/multiline=max-bytes=405,max-lines=2");
    const string prefix = ":" + identity.nick + "!user@localhost ";
    // A local-only parent has no remote id even on a fully capable connection.
    client.send("#reply", "local parent", "", "local-reply", "alice");
    const auto local = peer.expect("PRIVMSG", {"#reply", "alice: local parent"});
    require(!local.tags.contains("+reply") && local.tags.at("label") == "local-reply",
            "Local-only reply acquired a wire id or lost submission identity");
    peer.send("@msgid=local-echo " + prefix + "PRIVMSG #reply :alice: local parent");
    const auto echo =
        waitEvent(client, events, 0, [](const Event &event) { return event.id == "local-echo"; });
    require(events[echo].label == "local-reply" && events[echo].own,
            "Prefixed reply lost unlabeled echo correlation");

    // A capable reply is tagged, never prefixed, and the raw paste fits multiline.
    const string draft = string(398, 'x') + "\n" + "y";
    client.send("#reply", draft, "remote-parent", "retry-reply", "alice");
    const auto opening = peer.read();
    require(opening.command == "BATCH" && opening.params.size() == 3 &&
                opening.params[1] == "draft/multiline" &&
                opening.tags.at("+reply") == "remote-parent" &&
                opening.tags.at("label") == "retry-reply",
            "Capable reply did not select a tagged multiline batch");
    const string batch = opening.params[0].substr(1);
    peer.expect("PRIVMSG", {"#reply", string(398, 'x')});
    peer.expect("PRIVMSG", {"#reply", "y"});
    peer.expect("BATCH", {"-" + batch});
    // Revoke after admission: the retained raw request must choose the live
    // fallback when the server rejects its queued multiline representation.
    peer.send(":server CAP " + identity.nick +
              " DEL :message-tags\r\n@label=retry-reply "
              ":server FAIL BATCH MULTILINE_INVALID :retry as legacy");
    const string prefixed = "alice: " + draft;
    const vector<string> expected{prefixed.substr(0, 400), prefixed.substr(400, 5), "y"};
    for (const auto &text : expected) {
        const auto part = peer.expect("PRIVMSG", {"#reply", text});
        require(!part.tags.contains("+reply"), "Revoked reply retained its client tag");
    }
    waitEvent(client, events, 0, [](const Event &event) {
        return tag(event, "submission-complete") == "1" && event.label == "retry-reply.part2";
    });
    for (size_t i = 0; i < expected.size(); ++i) {
        const string label = i ? "retry-reply.part" + std::to_string(i) : "retry-reply";
        require(std::count_if(events.begin(), events.end(),
                              [&](const Event &event) {
                                  return tag(event, "pending") == "1" && event.label == label &&
                                         event.text == expected[i] &&
                                         tag(event, "submission") == "retry-reply";
                              }) == 1,
                "Fallback retry duplicated a prefix, ghost, or submission part");
        const auto pending = std::find_if(events.begin(), events.end(), [&](const Event &event) {
            return tag(event, "pending") == "1" && event.label == label &&
                   event.text == expected[i];
        });
        require(pending != events.end() && (i ? !pending->tags.contains("original-text")
                                              : tag(*pending, "original-text") == string(393, 'x')),
                "Fallback retry ghost lost its raw recovery component");
    }
    client.send("#reply", "unsupported local", "", "unsupported-local", "alice");
    const auto unsupported = peer.expect("PRIVMSG", {"#reply", "alice: unsupported local"});
    require(!unsupported.tags.contains("+reply"),
            "Unsupported local-only reply acquired a reply tag");
    // The nickname prefix participates in UTF-8-safe component boundaries.
    client.send("#reply", string(392, 'x') + "\xf0\x9f\x98\x80", "", "utf8-reply", "alice");
    peer.expect("PRIVMSG", {"#reply", "alice: " + string(392, 'x')});
    peer.expect("PRIVMSG", {"#reply", "\xf0\x9f\x98\x80"});
    // Prefix bytes count against multiline limits before choosing its encoding.
    peer.send(":server CAP " + identity.nick + " NEW :message-tags");
    peer.expect("CAP", {"REQ", "message-tags"});
    peer.send(":server CAP " + identity.nick + " ACK :message-tags");
    peer.fence();
    client.send("#reply", draft, "", "bounded-reply", "alice");
    for (const auto &text : expected)
        peer.expect("PRIVMSG", {"#reply", text});
    client.send("#reply", "one\ntwo", "", "fallback-multiline", "alice");
    const auto fallbackBatch = peer.read();
    require(fallbackBatch.command == "BATCH" && fallbackBatch.params.size() == 3 &&
                !fallbackBatch.tags.contains("+reply"),
            "In-bounds fallback did not use an untagged multiline batch");
    peer.expect("PRIVMSG", {"#reply", "alice: one"});
    peer.expect("PRIVMSG", {"#reply", "two"});
    peer.expect("BATCH", {"-" + fallbackBatch.params[0].substr(1)});
    peer.send("@label=fallback-multiline :server FAIL BATCH MULTILINE_INVALID :legacy");
    peer.expect("PRIVMSG", {"#reply", "alice: one"});
    peer.expect("PRIVMSG", {"#reply", "two"});
    peer.send("@label=fallback-multiline :server FAIL PRIVMSG INVALID_TARGET :rejected");
    const auto failure = waitEvent(client, events, 0, [](const Event &event) {
        return event.label == "fallback-multiline" && tag(event, "send-failed") == "1";
    });
    require(tag(events[failure], "original-text") == "one",
            "Failure recovery retained the synthetic nickname prefix");
    client.send("#reply", "raw recovery", "", "invalid-fallback", "bad nick");
    const auto invalid = waitEvent(client, events, 0, [](const Event &event) {
        return event.label == "invalid-fallback" && tag(event, "send-failed") == "1";
    });
    require(tag(events[invalid], "original-text") == "raw recovery",
            "Invalid fallback nickname mutated recovered raw text");
    waitEvent(client, events, 0, [](const Event &event) {
        return event.type == "sent" && event.label == "fallback-multiline.part1";
    });
    for (const auto &event : events)
        if (event.type == "sent")
            require(tag(event, "echo-expected") == "1",
                    "Echo-capable multiline/retry completion lost its admission snapshot");
    peer.fence();
    client.stop();
}

void renameChecks(SSL_CTX *context, const Identity &identity) {
    WirePeer peer(context);
    NetworkClient client(server(peer.port), identity);
    vector<Event> events;
    registerClient(peer, client, events, identity,
                   "message-tags echo-message draft/channel-rename batch draft/chathistory "
                   "draft/event-playback");
    const string prefix = ":" + identity.nick + "!user@localhost ";
    client.send("#old", "rename-wire-message", "", "caller-submission");
    const auto sent = peer.expect("PRIVMSG", {"#old", "rename-wire-message"});
    require(!sent.tags.contains("label"), "Unlabeled echo scenario unexpectedly has a wire label");
    const auto &ghost = events[waitEvent(client, events, 0, [](const Event &event) {
        return event.type == "message" && tag(event, "pending") == "1";
    })];
    require(ghost.target == "#old" && ghost.label == "caller-submission" &&
                tag(ghost, "submission") == "caller-submission",
            "Caller submission ghost was lost");
    client.react("#old", "rejected-reaction", ":)", false);
    const auto reaction = peer.expect("TAGMSG", {"#old"});
    require(!reaction.tags.contains("label") && reaction.tags.at("+draft/react") == ":)" &&
                reaction.tags.at("+reply") == "rejected-reaction",
            "Incorrect unlabeled reaction wire tags");
    // Deployed Ergo omits an empty reason; the explicit-empty form remains valid.
    peer.send(prefix + "RENAME #old #middle");
    peer.send(prefix + "RENAME #middle #new :");
    peer.send("@msgid=renamed-echo " + prefix + "PRIVMSG #new :rename-wire-message");
    const auto &echo = events[waitEvent(client, events, 0, [](const Event &event) {
        return event.type == "message" && event.id == "renamed-echo";
    })];
    require(echo.target == "#new" && echo.own && !echo.history && tag(echo, "pending").empty() &&
                echo.label == "caller-submission",
            "Live RENAME lost unlabeled own-send correlation");
    require(std::any_of(events.begin(), events.end(),
                        [](const Event &event) {
                            return tag(event, "kind") == "rename" && !event.history &&
                                   event.params == vector<string>{"#old", "#middle"} &&
                                   event.text.empty();
                        }),
            "Omitted-reason live RENAME lost its raw params or acquired a fake reason");
    require(std::any_of(events.begin(), events.end(),
                        [](const Event &event) {
                            return tag(event, "kind") == "rename" && !event.history &&
                                   event.params == vector<string>{"#middle", "#new", ""} &&
                                   event.text.empty();
                        }),
            "Explicit empty-reason live RENAME was not delivered");
    peer.send(":server 404 " + identity.nick + " #new :Cannot send reaction to channel");
    const auto &rollback = events[waitEvent(client, events, 0, [](const Event &event) {
        return event.type == "reaction" && tag(event, "rollback") == "1";
    })];
    require(rollback.target == "#new" && rollback.id == "rejected-reaction" && rollback.own &&
                tag(rollback, "remove") == "1",
            "Reaction rollback retained the old target");

    client.react("#new", "toggle-reaction", ":)", false);
    const auto added = peer.expect("TAGMSG", {"#new"});
    require(added.tags.at("+draft/react") == ":)", "Reaction did not add before historical RENAME");
    peer.send("@+draft/react=:);+reply=toggle-reaction " + prefix + "TAGMSG #new");
    peer.send(prefix + "RENAME #new #NEW :case-only rename");
    waitEvent(client, events, 0, [](const Event &event) {
        return tag(event, "kind") == "rename" && !event.history &&
               event.params == vector<string>{"#new", "#NEW", "case-only rename"} &&
               event.text == "case-only rename";
    });
    peer.send(":server BATCH +old-rename chathistory #new");
    peer.send("@batch=old-rename " + prefix + "RENAME #new #archived :old rename");
    peer.send(":server BATCH -old-rename");
    waitEvent(client, events, 0, [](const Event &event) {
        return tag(event, "kind") == "rename" && event.history && event.params[1] == "#archived" &&
               event.text == "old rename";
    });
    client.react("#new", "toggle-reaction", ":)", true);
    const auto removed = peer.expect("TAGMSG", {"#new"});
    require(removed.tags.at("+draft/unreact") == ":)" && !removed.tags.contains("+draft/react"),
            "Historical RENAME migrated the live own-reaction toggle");
    peer.send("@+draft/unreact=:);+reply=toggle-reaction " + prefix + "TAGMSG #new");
    peer.fence();
    client.stop();
    collect(client, events);
    require(std::none_of(events.begin(), events.end(),
                         [](const Event &event) { return tag(event, "send-failed") == "1"; }),
            "Confirmed renamed own send failed when the worker stopped");
}
void sentChecks(SSL_CTX *context, const Identity &identity) {
    WirePeer peer(context);
    NetworkClient client(server(peer.port), identity);
    vector<Event> events;
    registerClient(peer, client, events, identity, "message-tags batch");
    peer.send(":server BATCH +sent-hold draft/multiline #held\r\n"
              "@batch=sent-hold :alice!user@localhost PRIVMSG #held :held message");
    peer.fence();
    client.send("#write", string(401, 'x'), "", "write-submission");
    peer.expect("PRIVMSG", {"#write", string(400, 'x')});
    peer.expect("PRIVMSG", {"#write", "x"});
    waitEvent(client, events, 0, [](const Event &event) {
        return event.type == "sent" && event.label == "write-submission.part1";
    });
    for (const string label : {"write-submission", "write-submission.part1"}) {
        const auto ghost = std::find_if(events.begin(), events.end(), [&](const Event &event) {
            return event.label == label && tag(event, "pending") == "1";
        });
        const auto sent = std::find_if(events.begin(), events.end(), [&](const Event &event) {
            return event.type == "sent" && event.label == label;
        });
        require(ghost != events.end() && sent != events.end() && ghost < sent && sent->own &&
                    sent->server == "wire-check",
                "Full-write status did not follow its exact admitted ghost");
    }
    require(std::none_of(events.begin(), events.end(),
                         [](const Event &event) { return event.text == "held message"; }),
            "Sent fixture unexpectedly released the unrelated incoming batch");
    client.send("#write", "invalid\rtext", "", "invalid-write");
    waitEvent(client, events, 0, [](const Event &event) {
        return tag(event, "send-failed") == "1" && event.label == "invalid-write";
    });
    peer.send(":server BATCH -sent-hold");
    peer.fence();
    client.stop();
    collect(client, events);
    require(std::count_if(events.begin(), events.end(),
                          [](const Event &event) { return event.type == "sent"; }) == 2,
            "Registration, control traffic or shutdown produced extra sent events");
}

void blockedSentChecks(SSL_CTX *context, const Identity &identity, bool multiline, bool disconnect,
                       bool reject = false) {
    WirePeer peer(context);
    NetworkClient client(server(peer.port), identity);
    vector<Event> events;
    registerClient(peer, client, events, identity,
                   multiline ? "message-tags batch standard-replies labeled-response echo-message "
                               "draft/multiline=max-bytes=2000000,max-lines=4096"
                             : "message-tags batch");
    const int clientSocket = peer.limitClientWrites();
    constexpr size_t parts = 4096;
    const string submission = multiline ? "blocked-multiline" : "blocked-legacy";
    client.send("#write", string(400 * parts, 'x'), "", submission);
    auto ghost = waitEvent(client, events, 0, [&](const Event &event) {
        return tag(event, "submission-complete") == "1" && tag(event, "submission") == submission;
    });
    string batch;
    if (multiline) {
        const auto opening = peer.read();
        require(opening.command == "BATCH" && opening.params.size() == 3 &&
                    opening.params[0].starts_with("+") && opening.params[1] == "draft/multiline" &&
                    opening.tags.at("label") == submission,
                "Multiline full-write fixture did not open its labeled batch");
        batch = opening.params[0].substr(1);
    }
    peer.expect("PRIVMSG", {"#write", string(400, 'x')});
    // Reading a component proves transport progress, but bounded socket buffers
    // cannot hold the entire 1.6 MiB submission while this peer stops draining.
    if (!multiline)
        waitEvent(client, events, 0, [&](const Event &event) {
            return event.type == "sent" && event.label == submission;
        });
    collect(client, events);
    const auto sentCount = [&] {
        return static_cast<size_t>(std::count_if(
            events.begin(), events.end(), [](const Event &event) { return event.type == "sent"; }));
    };
    require(multiline ? sentCount() == 0 : sentCount() > 0 && sentCount() < parts,
            "Admission or partial transport progress was reported as complete");
    if (reject) {
        const auto from = events.size();
        peer.send("@label=" + submission +
                  " :server FAIL BATCH MULTILINE_INVALID :retry as legacy");
        ghost = waitEvent(client, events, from, [&](const Event &event) {
            return tag(event, "submission-complete") == "1" &&
                   event.label == submission + ".part4095";
        });
        require(sentCount() == 0,
                "Rejected blocked multiline completed before replacement frames were written");
    }
    if (disconnect) {
        const linger reset{1, 0};
        require(::setsockopt(peer.connection.value, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)) ==
                    0,
                "Cannot arrange deterministic loopback reset");
        peer.connection.reset();
        waitEvent(client, events, 0, [](const Event &event) {
            return event.type == "state" && event.text == "Disconnected";
        });
        client.stop();
        collect(client, events);
        require(multiline ? sentCount() == 0 : sentCount() > 0 && sentCount() < parts,
                "Failed incomplete transport emitted a completion");
        const string unfinished = multiline ? submission : submission + ".part4095";
        require(std::any_of(events.begin(), events.end(),
                            [&](const Event &event) {
                                return tag(event, "send-failed") == "1" &&
                                       event.label == unfinished;
                            }) &&
                    std::none_of(events.begin(), events.end(),
                                 [&](const Event &event) {
                                     return event.type == "sent" && event.label == unfinished;
                                 }),
                "Incomplete final ghost lost its failure boundary");
        size_t completed = 0;
        for (const auto &event : events) {
            if (event.type != "sent")
                continue;
            const string expected =
                completed == 0 ? submission : submission + ".part" + std::to_string(completed);
            require(event.own && event.label == expected,
                    "Partial writes lost their exact legacy ghost labels or emitted duplicates");
            ++completed;
        }
        return;
    }
    const int bytes = 4 * 1024 * 1024;
    require(::setsockopt(clientSocket, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes)) == 0 &&
                ::setsockopt(peer.connection.value, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes)) ==
                    0,
            "Cannot restore loopback throughput");
    for (size_t i = 1; i < parts; ++i)
        peer.expect("PRIVMSG", {"#write", string(400, 'x')});
    peer.expect("BATCH", {"-" + batch});
    if (reject)
        for (size_t i = 0; i < parts; ++i)
            peer.expect("PRIVMSG", {"#write", string(400, 'x')});
    const string completion = reject ? submission + ".part4095" : submission;
    const auto sent = waitEvent(client, events, 0, [&](const Event &event) {
        return event.type == "sent" && event.label == completion;
    });
    const size_t expectedCount = reject ? parts : 1;
    require(ghost < sent && events[sent].own && sentCount() == expectedCount,
            "Multiline or replacement emitted premature or duplicate completion");
    size_t completed = 0;
    for (const auto &event : events) {
        if (event.type != "sent")
            continue;
        const string expected =
            completed == 0 ? submission : submission + ".part" + std::to_string(completed);
        require(event.own && event.label == expected,
                "Rejected multiline closing frame completed a replacement ghost");
        ++completed;
    }
    peer.fence();
    client.stop();
    collect(client, events);
    require(sentCount() == expectedCount, "Multiline closing frame emitted duplicate completion");
}
void preReadyMetadataChecks(SSL_CTX *context, const Identity &identity) {
    WirePeer peer(context);
    NetworkClient client(server(peer.port), identity);
    vector<Event> events;
    Clock::time_point postponed;
    registerClient(
        peer, client, events, identity, "batch draft/metadata-2=max-subs=1", false, false, [&] {
            postponed = Clock::now();
            peer.send(":" + identity.nick + "!user@localhost JOIN #pre-ready\r\n" + ":server 774 " +
                      identity.nick + " #pre-ready 2");
            peer.expect("WHO", {"#pre-ready"});
            peer.fence();
            collect(client, events);
            require(std::none_of(events.begin(), events.end(),
                                 [](const Event &event) {
                                     return event.type == "state" && event.text == "Ready";
                                 }),
                    "Metadata replay fixture reached Ready before registration");
        });
    const auto sync = peer.read(postponed + 4s);
    require(sync.command == "METADATA" && sync.params == vector<string>{"#pre-ready", "SYNC"} &&
                Clock::now() >= postponed + 2s,
            "Ready discarded or shortened a pre-registration 774 RetryAfter");
    peer.fence();
    client.stop();
    collect(client, events);
}

void metadataLookupChecks(SSL_CTX *context, const Identity &identity) {
    WirePeer peer(context);
    NetworkClient client(server(peer.port), identity);
    vector<Event> events;
    registerClient(peer, client, events, identity, "batch draft/metadata-2=max-subs=0", false,
                   false, {}, false);
    const string prefix = ":" + identity.nick + "!user@localhost ";
    peer.send(prefix + "JOIN #lookup");
    peer.expect("WHO", {"#lookup"});
    peer.expect("METADATA", {"#lookup", "GET", "avatar"});
    peer.fence(); // max-subs=0 must never send SUB or SYNC.
    peer.send(":server BATCH +lookup metadata #lookup\r\n"
              "@batch=lookup :server 761 " +
              identity.nick +
              " #lookup avatar * :https://example.org/lookup.png\r\n:server BATCH -lookup");
    const auto &value = events[waitEvent(client, events, 0, [](const Event &event) {
        return tag(event, "kind") == "metadata" && tag(event, "command") == "761";
    })];
    require(value.target == "#lookup" &&
                value.params == vector<string>{identity.nick, "#lookup", "avatar", "*",
                                               "https://example.org/lookup.png"},
            "Subscription-free metadata GET response lost its channel target or value");
    peer.send(":server 774 " + identity.nick + " #lookup 1");
    peer.fence();
    peer.quietUntil(Clock::now() + 1100ms);

    peer.send(prefix + "PART #lookup\r\n:server CAP " + identity.nick +
              " DEL :draft/metadata-2\r\n" + prefix + "JOIN #late");
    peer.expect("WHO", {"#late"});
    peer.fence();
    peer.send(":server CAP " + identity.nick + " NEW :draft/metadata-2=max-subs=0");
    peer.expect("CAP", {"REQ", "draft/metadata-2"});
    peer.send(":server CAP " + identity.nick + " NAK :draft/metadata-2");
    peer.fence();
    peer.send(prefix + "JOIN #declined");
    peer.expect("WHO", {"#declined"});
    peer.fence(); // A declined capability must not enable GET timers either.
    peer.send(":server CAP " + identity.nick + " NEW :draft/metadata-2=max-subs=0");
    peer.expect("CAP", {"REQ", "draft/metadata-2"});
    peer.send(":server CAP " + identity.nick + " ACK :draft/metadata-2");
    peer.expect("METADATA", {"#declined", "GET", "avatar"});
    peer.expect("METADATA", {"#late", "GET", "avatar"});
    peer.fence(); // Already joined channels are fetched without SUB/SYNC.
    peer.send(":server CAP " + identity.nick + " ACK :draft/metadata-2");
    peer.fence(); // Duplicate ACK must not duplicate lookups.

    peer.send(":server CAP " + identity.nick + " DEL :batch\r\n" + prefix + "JOIN #batch-off");
    peer.expect("WHO", {"#batch-off"});
    peer.fence();
    peer.send(":server CAP " + identity.nick + " NEW :batch");
    peer.expect("CAP", {"REQ", "batch"});
    peer.send(":server CAP " + identity.nick + " ACK :batch");
    for (const auto &target : {"#batch-off", "#declined", "#late"})
        peer.expect("METADATA", {target, "GET", "avatar"});
    peer.fence(); // Both capabilities remain mandatory for subscription-free lookup.
    client.stop();
    collect(client, events);
}

void metadataChecks(SSL_CTX *context, const Identity &identity) {
    WirePeer peer(context);
    NetworkClient client(server(peer.port), identity);
    vector<Event> events;
    const string caps = "message-tags batch draft/channel-rename draft/chathistory "
                        "draft/event-playback standard-replies "
                        "draft/multiline=max-bytes=4096,max-lines=16";
    registerClient(peer, client, events, identity, caps);
    const string prefix = ":" + identity.nick + "!user@localhost ";
    auto join = [&](const string &target, bool metadata) {
        peer.send(prefix + "JOIN " + target);
        peer.expect("WHO", {target});
        if (metadata)
            peer.expect("METADATA", {target, "SYNC"});
    };
    auto enable = [&](const set<string> &targets) {
        peer.send(":server CAP " + identity.nick + " NEW :draft/metadata-2=max-subs=1");
        peer.expect("CAP", {"REQ", "draft/metadata-2"});
        peer.send(":server CAP " + identity.nick + " ACK :draft/metadata-2");
        peer.expect("METADATA", {"*", "SUB", "avatar"});
        for (const auto &target : targets)
            peer.expect("METADATA", {target, "SYNC"});
        peer.fence();
    };
    join("#before", false);
    join("#left-before", false);
    join("#kicked-before", false);
    peer.send(prefix + "PART #left-before");
    peer.send(":operator!user@localhost KICK #kicked-before " + identity.nick + " :removed");
    enable({"#before"});
    peer.send(":server CAP " + identity.nick + " ACK :draft/metadata-2");
    peer.fence(); // Duplicate ACK must not subscribe or synchronize twice.

    const auto from = events.size();
    peer.send(":server BATCH +avatars metadata #unrelated");
    const vector<vector<string>> responses = {
        {"#before", "avatar", "*", "https://example.org/channel.png"},
        {"#before", "avatar", "*"},
        {"#before", "avatar", "*", ""},
        {identity.nick, "#before", "avatar", "*", "https://example.org/numeric.png"},
        {identity.nick, "#before", "avatar", "*", ""},
        {identity.nick, "#before", "avatar", "Key is not set"}};
    for (size_t i = 0; i < responses.size(); ++i) {
        irc::WireMessage response;
        response.prefix = "server";
        response.command = i < 3 ? "METADATA" : i < 5 ? "761" : "766";
        response.params = responses[i];
        response.tags["batch"] = "avatars";
        peer.send(irc::serialize(response, irc::incomingCoreLimit));
    }
    peer.send("@batch=avatars :server METADATA #before avatar");
    peer.send("@batch=avatars :server METADATA #bad,target avatar * :invalid");
    peer.send("@batch=avatars :server 761 " + identity.nick + " #before avatar *");
    peer.send("@batch=avatars :server 761 othernick #before avatar * :wrong recipient");
    peer.send("@batch=avatars :server 766 " + identity.nick + " #before avatar");
    peer.send(":server BATCH -avatars");
    peer.fence();
    collect(client, events);
    size_t responseIndex = 0;
    for (size_t i = from; i < events.size(); ++i) {
        const auto &event = events[i];
        if (tag(event, "kind") != "metadata")
            continue;
        require(responseIndex < responses.size(), "Malformed metadata was routed as an update");
        const auto index = responseIndex++;
        require(event.type == "network" && event.target == "#before" &&
                    event.params == responses[index] && !event.history &&
                    tag(event, "command") == (index < 3   ? "METADATA"
                                              : index < 5 ? "761"
                                                          : "766") &&
                    event.text == (index == 0 || index == 3 ? responses[index].back() : ""),
                "Metadata routing lost numeric recipient offsets, deletion or empty value");
    }
    require(responseIndex == responses.size(), "A valid metadata response was not routed");

    for (const string cap : {"draft/metadata-2", "batch"}) {
        const auto heldFrom = events.size();
        peer.send(":server BATCH +avatar-cap-hold draft/multiline #before\r\n"
                  "@batch=avatar-cap-hold :alice!user@localhost PRIVMSG #before :held chat\r\n"
                  ":server METADATA #before avatar * :https://example.org/stale.png\r\n"
                  ":server 761 " +
                  identity.nick + " #before avatar * :https://example.org/stale-numeric.png\r\n" +
                  ":server 766 " + identity.nick + " #before avatar :stale deletion\r\n" +
                  ":server CAP " + identity.nick + " DEL :" + cap);
        peer.fence();
        peer.send(":server METADATA #before avatar * :https://example.org/disabled.png");
        peer.send(":server CAP " + identity.nick + " NEW :" + cap +
                  (cap == "draft/metadata-2" ? "=max-subs=1" : ""));
        peer.expect("CAP", {"REQ", cap});
        peer.send(":server CAP " + identity.nick + " ACK :" + cap);
        peer.expect("METADATA", {"*", "SUB", "avatar"});
        peer.expect("METADATA", {"#before", "SYNC"});
        peer.send(":server METADATA #before avatar * :https://example.org/fresh.png\r\n"
                  ":server BATCH -avatar-cap-hold");
        peer.fence();
        collect(client, events);
        size_t updates = 0, chat = 0;
        for (size_t i = heldFrom; i < events.size(); ++i) {
            const auto &event = events[i];
            if (tag(event, "kind") == "metadata") {
                ++updates;
                require(event.target == "#before" && event.text == "https://example.org/fresh.png",
                        "Held or disabled metadata crossed a capability reset");
            }
            if (event.type == "message" && event.text == "held chat")
                ++chat;
        }
        require(updates == 1 && chat == 1,
                "Capability reset lost fresh metadata or unrelated held chat");
    }

    for (const auto &target : {"#retry", "#default", "#rename", "#cancel-part", "#cancel-kick",
                               "#huge", "#invalid", "#[fold]"})
        join(target, true);
    peer.fence();
    // All server deadlines start after this timestamp; no test clock or fast path.
    const auto postponed = Clock::now();
    peer.send(":server 005 " + identity.nick + " CASEMAPPING=ascii :are supported\r\n" +
              ":server 774 " + identity.nick + " #retry 2\r\n" + ":server 774 " + identity.nick +
              " #retry 1\r\n" + ":server 774 " + identity.nick + " #default\r\n" + ":server 774 " +
              identity.nick + " #rename 2\r\n" + ":server 774 " + identity.nick + " #[fold] 2\r\n" +
              ":server 774 " + identity.nick + " #{fold} 10\r\n" + ":server 774 " + identity.nick +
              " #cancel-part 1\r\n" + ":server 774 " + identity.nick + " #cancel-kick 1\r\n" +
              ":server 774 " + identity.nick + " #huge 999999999999999999999999999999\r\n" +
              ":server 774 " + identity.nick + " #invalid 0\r\n" + ":server 774 " + identity.nick +
              " #invalid -1\r\n" + ":server 774 " + identity.nick + " #invalid 1x\r\n" +
              ":server 774 " + identity.nick + " #invalid 1 extra\r\n" + ":server 774 " +
              identity.nick + " alice 1\r\n" + ":server 774 " + identity.nick +
              " #not-joined 1\r\n" + ":server 774 " + identity.nick + " #bad,target 1\r\n" +
              ":server 774 " + identity.nick + " @#before 1\r\n" +
              ":server 774 othernick #before 1\r\n" + prefix + "PART #cancel-part\r\n" +
              ":operator!user@localhost KICK #cancel-kick " + identity.nick + " :removed\r\n" +
              ":alice!user@localhost PART #retry\r\n" +
              ":operator!user@localhost KICK #retry alice :another member\r\n" +
              ":server RENAME #rename #renamed\r\n" + ":server RENAME #renamed #RENAMED\r\n" +
              ":server BATCH +old-metadata chathistory #RENAMED\r\n" +
              "@batch=old-metadata :server RENAME #RENAMED #archived\r\n" +
              "@batch=old-metadata :server 774 " + identity.nick + " #retry 30\r\n" +
              ":server BATCH -old-metadata");
    peer.fence();
    set<string> due{"#retry", "#RENAMED", "#[fold]"};
    while (!due.empty()) {
        const auto sync = peer.read(postponed + 4s);
        require(sync.command == "METADATA" && sync.params.size() == 2 && sync.params[1] == "SYNC" &&
                    due.erase(sync.params[0]) == 1 && Clock::now() >= postponed + 2s,
                "774 retried early, duplicated, ignored CASEMAPPING or lost a live rename");
    }
    const auto repeated = Clock::now();
    peer.send(":server 774 " + identity.nick + " #retry 1");
    peer.expect("METADATA", {"#retry", "SYNC"});
    require(Clock::now() >= repeated + 1s, "Repeated 774 did not wait its new RetryAfter");
    const auto fallback = peer.read(postponed + 7s);
    require(fallback.command == "METADATA" &&
                fallback.params == vector<string>{"#default", "SYNC"} &&
                Clock::now() >= postponed + 5s,
            "774 without RetryAfter did not use the five-second default");
    peer.fence(); // No canceled, malformed, user, unjoined, huge or duplicate retry.

    peer.send(":server 774 " + identity.nick + " #before 1\r\n" + ":server CAP " + identity.nick +
              " ACK :-draft/metadata-2");
    peer.fence();
    peer.quietUntil(Clock::now() + 1100ms);
    // Batch and metadata may ACK in either order; neither alone may issue SUB.
    peer.send(":server CAP " + identity.nick + " DEL :batch draft/metadata-2");
    peer.fence();
    peer.send(":server CAP " + identity.nick + " NEW :batch draft/metadata-2=max-subs=1");
    peer.expect("CAP", {"REQ", "batch draft/metadata-2"});
    peer.send(":server CAP " + identity.nick + " ACK :draft/metadata-2");
    peer.fence();
    peer.send(":server CAP " + identity.nick + " ACK :batch");
    peer.expect("METADATA", {"*", "SUB", "avatar"});
    const set<string> remaining{"#RENAMED", "#[fold]",  "#before", "#default",
                                "#huge",    "#invalid", "#retry"};
    for (const auto &target : remaining)
        peer.expect("METADATA", {target, "SYNC"});
    peer.fence();
    peer.send(":server 774 " + identity.nick + " #before 1\r\n" + ":server CAP " + identity.nick +
              " DEL :batch");
    peer.fence();
    peer.quietUntil(Clock::now() + 1100ms);
    peer.send(":server CAP " + identity.nick + " NEW :batch");
    peer.expect("CAP", {"REQ", "batch"});
    peer.send(":server CAP " + identity.nick + " ACK :batch");
    peer.expect("METADATA", {"*", "SUB", "avatar"});
    for (const auto &target : remaining)
        peer.expect("METADATA", {target, "SYNC"});
    peer.fence();
    peer.send(":server 774 " + identity.nick + " #before 1\r\n" + ":server CAP " + identity.nick +
              " DEL :draft/metadata-2");
    peer.fence();
    peer.quietUntil(Clock::now() + 1100ms);

    enable(remaining);
    peer.send(":server 774 " + identity.nick + " #before 1");
    peer.fence();
    client.stop();
    collect(client, events);
    events.clear();
    registerClient(peer, client, events, identity, caps);
    enable({}); // A new session has no remembered membership or retry deadlines.
    peer.quietUntil(Clock::now() + 1100ms);
    client.stop();
    collect(client, events);
}

void avatarSmoke(SSL_CTX *context, const Identity &identity, const fs::path &certificate,
                 const string &url, const fs::path &output) {
    WirePeer peer(context);
    NetworkClient client(server(peer.port), identity);
    vector<Event> events;
    {
        Environment trust("SSL_CERT_FILE", certificate);
        Environment nixTrust("NIX_SSL_CERT_FILE", certificate);
        registerClient(peer, client, events, identity, "batch draft/metadata-2=max-subs=1");
    } // Public avatar HTTPS uses the caller's original trust configuration.
    peer.send(":" + identity.nick + "!user@localhost JOIN #avatar");
    peer.expect("WHO", {"#avatar"});
    peer.expect("METADATA", {"#avatar", "SYNC"});
    irc::WireMessage response;
    response.prefix = "server";
    response.command = "761";
    response.params = {identity.nick, "#avatar", "avatar", "*", url};
    peer.send(irc::serialize(response, irc::incomingCoreLimit));
    const auto &metadata = events[waitEvent(client, events, 0, [](const Event &event) {
        return event.type == "network" && tag(event, "kind") == "metadata" &&
               tag(event, "command") == "761" && event.target == "#avatar";
    })];
    require(metadata.params == response.params, "Runtime avatar metadata did not retain its URL");
    AvatarLoader loader;
    constexpr std::uint64_t request = 1;
    require(loader.request(request, metadata.params[4]) == AvatarLoader::Admission::Queued,
            "AvatarLoader rejected runtime avatar URL");
    const auto deadline = Clock::now() + 30s;
    for (;;) {
        for (const auto &result : loader.poll())
            if (result.request == request) {
                if (result.path.empty())
                    throw std::runtime_error("Runtime avatar fetch failed: " + result.error);
                require(fs::is_regular_file(result.path) && fs::file_size(result.path) > 0,
                        "Runtime avatar fetch returned a missing or empty cache file");
                fs::copy_file(result.path, output, fs::copy_options::overwrite_existing);
                std::cout << "avatar-smoke: TLS batch+draft/metadata-2; JOIN #avatar; 761 routed; "
                          << fs::file_size(output) << " fetched bytes copied to " << output << '\n';
                client.stop();
                collect(client, events);
                return;
            }
        require(Clock::now() < deadline, "Runtime avatar fetch deadline expired");
        std::this_thread::sleep_for(1ms);
    }
}

void historyConsumerWireChecks(SSL_CTX *context, const Identity &identity, bool labeled) {
    WirePeer peer(context);
    NetworkClient client(server(peer.port), identity);
    vector<Event> events;
    registerClient(peer, client, events, identity,
                   "message-tags server-time batch draft/chathistory" +
                       string(labeled ? " labeled-response" : ""));
    for (const string numeric : {"376", "422"}) {
        const auto from = events.size();
        const string text = "history capability settlement " + numeric;
        peer.send(":server " + numeric + " " + identity.nick + " :" + text);
        const auto marker = waitEvent(client, events, from, [](const Event &e) {
            return e.type == "network" && tag(e, "kind") == "isupport-settled";
        });
        require(events[marker].params == vector<string>{identity.nick, text} &&
                    !events[marker].tags.contains("CHANTYPES"),
                "History capability settlement fabricated ISUPPORT tokens or lost numeric params");
        waitEvent(client, events, from, [&](const Event &e) {
            return e.type == "network" && tag(e, "kind") == "raw" &&
                   e.params == vector<string>{identity.nick, text};
        });
        require(std::none_of(events.begin() + from, events.end(),
                             [](const Event &e) {
                                 return e.type == "network" && tag(e, "kind") == "isupport" &&
                                        e.tags.contains("CHANTYPES");
                             }),
                "MOTD terminal numeric fabricated CHANTYPES advertisement");
    }
    auto prefix = [&](const string &id) { return labeled ? "@label=" + id + " " : ""; };
    auto end = [&](const string &id) {
        const auto index = waitEvent(client, events, 0, [&](const Event &e) {
            return e.type == "history_end" && e.label == id;
        });
        return events[index];
    };
    auto latest = [&](const string &id) {
        client.history({.target = "#bounds", .requestId = id});
        peer.expect("CHATHISTORY", {"LATEST", "#bounds", "*", "50"});
    };
    const string time = "2026-11-01T12:00:00.123Z";
    latest("ordered");
    if (!labeled) {
        const auto liveStart = events.size();
        peer.send(":server BATCH +live-join netjoin old.server new.server\r\n"
                  "@batch=live-join :LivePeer!u@h JOIN #live\r\n"
                  ":server BATCH -live-join\r\n"
                  ":server BATCH +live-split netsplit old.server new.server\r\n"
                  "@batch=live-split :LivePeer!u@h QUIT :split\r\n"
                  ":server BATCH -live-split");
        waitEvent(client, events, liveStart,
                  [](const Event &e) { return e.type == "quit" && e.nick == "LivePeer"; });
        for (const auto &type : {"join", "quit"})
            require(std::any_of(events.begin() + liveStart, events.end(),
                                [&](const Event &e) {
                                    return e.type == type && e.nick == "LivePeer" && !e.history &&
                                           e.label.empty();
                                }),
                    "Unrelated live presence inherited pending history semantics");
        require(std::none_of(
                    events.begin() + liveStart, events.end(),
                    [](const Event &e) { return e.type == "history_end" && e.label == "ordered"; }),
                "Unrelated live batch completed pending unlabeled history");
    }
    peer.send(prefix("ordered") +
              ":server BATCH +ordered chathistory #bounds\r\n"
              "@batch=ordered;msgid=z-old;time=" +
              time +
              " :Peer!u@h PRIVMSG #bounds :first\r\n"
              "@batch=ordered;msgid=a-new;time=" +
              time + " :Peer!u@h PRIVMSG #bounds :last\r\n:server BATCH -ordered");
    const auto ordered = end("ordered");
    require(tag(ordered, "error").empty() && tag(ordered, "count") == "2" &&
                tag(ordered, "oldest-id") == "z-old" && tag(ordered, "newest-id") == "a-new" &&
                tag(ordered, "oldest-time") == time && tag(ordered, "newest-time") == time,
            "History bounds sorted opaque IDs or lost tied response-order timestamps");
    for (const bool identified : {true, false}) {
        const string owner = identified ? "mutation-ids" : "mutation-time";
        latest(owner);
        peer.send(prefix(owner) +
                  ":server BATCH +mutations chathistory #bounds\r\n"
                  "@batch=mutations;+draft/react=:);+reply=reacted-message;time=" +
                  time + (identified ? ";msgid=reaction-wire-id" : "") +
                  " :Peer!u@h TAGMSG #bounds\r\n"
                  "@batch=mutations;time=" +
                  time + (identified ? ";msgid=redaction-wire-id" : "") +
                  " :Peer!u@h REDACT #bounds redacted-message :removed\r\n"
                  ":server BATCH -mutations");
        const auto mutationEnd = end(owner);
        require(tag(mutationEnd, "error").empty() && tag(mutationEnd, "count") == "2" &&
                    tag(mutationEnd, "oldest-id") == (identified ? "reaction-wire-id" : "") &&
                    tag(mutationEnd, "newest-id") == (identified ? "redaction-wire-id" : "") &&
                    tag(mutationEnd, "oldest-time") == time &&
                    tag(mutationEnd, "newest-time") == time,
                "Mutation boundaries used target IDs instead of their own wire IDs/time");
        for (const string type : {"reaction", "redact"})
            require(std::any_of(events.begin(), events.end(),
                                [&](const Event &e) {
                                    return e.type == type && e.history &&
                                           tag(e, "history-owner") == owner &&
                                           e.id == (type == "reaction" ? "reacted-message"
                                                                       : "redacted-message");
                                }),
                    "History boundary accounting changed the mutation consumer target ID");
    }
    latest("page-owner");
    peer.send(prefix("page-owner") +
              ":server BATCH +owner-page chathistory #bounds\r\n"
              "@batch=owner-page;label=original-send;msgid=owner-first;time=" +
              time +
              ";history-owner=wire-spoof;history-standalone=1 "
              ":Self!u@h PRIVMSG #bounds :owned direct\r\n"
              "@batch=owner-page;history-owner=context-spoof;history-standalone=1 "
              ":server BATCH +owner-lines draft/multiline #bounds\r\n"
              "@batch=owner-lines;label=original-multiline;msgid=owner-last;time=" +
              time +
              ";history-owner=fragment-spoof;history-standalone=1 "
              ":Self!u@h PRIVMSG #bounds :owned multiline\r\n"
              ":server BATCH -owner-lines\r\n:server BATCH -owner-page");
    const auto ownedEnd = end("page-owner");
    require(tag(ownedEnd, "error").empty() && tag(ownedEnd, "count") == "2" &&
                tag(ownedEnd, "oldest-id") == "owner-first" &&
                tag(ownedEnd, "newest-id") == "owner-last",
            "Original send labels stole page ownership or count/bounds");
    for (const string id : {"owner-first", "owner-last"})
        require(std::any_of(events.begin(), events.end(),
                            [&](const Event &e) {
                                return e.type == "message" && e.id == id && e.history &&
                                       e.label == (id == "owner-first" ? "original-send"
                                                                       : "original-multiline") &&
                                       tag(e, "history-owner") == "page-owner" &&
                                       !e.tags.contains("history-standalone");
                            }),
                "History ownership did not survive original label/multiline spoofed context");
    peer.send("@history-owner=forged;history-standalone=1;msgid=spoof-live "
              ":Peer!u@h PRIVMSG #bounds :untrusted wire markers");
    const auto spoof = waitEvent(client, events, 0, [](const Event &e) {
        return e.type == "message" && e.id == "spoof-live";
    });
    require(!events[spoof].history && !events[spoof].tags.contains("history-owner") &&
                !events[spoof].tags.contains("history-standalone"),
            "Arbitrary wire markers acquired trusted history ownership");
    if (labeled) {
        peer.send("@label=retired-history :server BATCH +retired chathistory #bounds\r\n"
                  "@batch=retired;history-owner=retired-history;history-standalone=1;"
                  "msgid=retired-message :Peer!u@h PRIVMSG #bounds :stale page\r\n"
                  ":server BATCH -retired");
        const auto stale = waitEvent(client, events, 0, [](const Event &e) {
            return e.type == "message" && e.id == "retired-message";
        });
        require(events[stale].history && tag(events[stale], "history-owner") == "retired-history" &&
                    !events[stale].tags.contains("history-standalone"),
                "Stale explicitly labeled root lost its rejecting owner identity");
        require(std::none_of(events.begin(), events.end(),
                             [](const Event &e) {
                                 return e.type == "history_begin" && e.label == "retired-history" &&
                                        e.tags.contains("unsolicited");
                             }),
                "Stale explicitly labeled root was admitted as unsolicited playback");
    }

    auto targets = [&](const string &id) {
        client.history({.direction = HistoryDirection::Targets,
                        .first = {"", "2026-11-02T00:00:00Z"},
                        .second = {"", "2026-10-01T00:00:00Z"},
                        .requestId = id,
                        .limit = 100});
        peer.expect("CHATHISTORY", {"TARGETS", "timestamp=2026-11-02T00:00:00.000Z",
                                    "timestamp=2026-10-01T00:00:00.000Z", "100"});
    };
    const auto targetsStart = events.size();
    targets("targets");
    peer.send(
        string(labeled ? "@label=targets;draft/chathistory-end " : "@draft/chathistory-end ") +
        ":server BATCH +targets draft/chathistory-targets\r\n"
        "@batch=targets :server CHATHISTORY TARGETS #Room "
        "2026-11-01T13:00:00.123999999+01:00\r\n"
        "@batch=targets :server CHATHISTORY TARGETS #Room " +
        time + "\r\n:server BATCH -targets");
    const auto targetEnd = end("targets");
    require(tag(targetEnd, "error").empty() && tag(targetEnd, "direction") == "targets" &&
                tag(targetEnd, "count") == "2" && tag(targetEnd, "end") == "1",
            "TARGETS lost raw record count or opening end tag");
    require(std::count_if(events.begin() + targetsStart, events.end(),
                          [&](const Event &e) {
                              return e.type == "history_target" && e.label == "targets" &&
                                     e.target == "#Room" && e.time == time;
                          }) == 2,
            "TARGETS lost correlation or failed UTC/millisecond truncation normalization");
    for (const string kind :
         {"wrong-root", "bad-time", "bad-target", "missing-record-field", "utc-overflow"}) {
        if (!labeled && kind == "wrong-root")
            continue; // Unlabeled unrelated root types cannot identify this request.
        targets(kind);
        const string type = kind == "wrong-root" ? "chathistory" : "draft/chathistory-targets";
        string record;
        if (kind == "bad-time")
            record = "@batch=broken :server CHATHISTORY TARGETS #Room not-a-time\r\n";
        else if (kind == "bad-target")
            record = "@batch=broken :server CHATHISTORY TARGETS :invalid target " + time + "\r\n";
        else if (kind == "missing-record-field")
            record = "@batch=broken :server CHATHISTORY TARGETS #Room\r\n";
        else if (kind == "utc-overflow")
            record = "@batch=broken :server CHATHISTORY TARGETS #Room "
                     "9999-12-31T23:59:59-01:00\r\n";
        peer.send(prefix(kind) + ":server BATCH +broken " + type + "\r\n" + record +
                  ":server BATCH -broken");
        const auto failed = end(kind);
        require(!tag(failed, "error").empty() && tag(failed, "end").empty(),
                "Malformed TARGETS response fabricated successful empty discovery");
        require(std::none_of(
                    events.begin(), events.end(),
                    [&](const Event &e) { return e.type == "history_target" && e.label == kind; }),
                "Malformed TARGETS record reached its consumer");
    }
    if (labeled) {
        latest("missing-root");
        peer.send("@label=missing-root :server BATCH +missing labeled-response\r\n"
                  "@batch=missing :server ACK\r\n:server BATCH -missing");
        require(!tag(end("missing-root"), "error").empty(),
                "Labeled response without history root fabricated empty success");
    }
    if (labeled) {
        latest("wrong-message-root");
        peer.send(prefix("wrong-message-root") +
                  ":server BATCH +wrong-message draft/chathistory-targets\r\n"
                  ":server BATCH -wrong-message");
        require(!tag(end("wrong-message-root"), "error").empty(),
                "Message page accepted TARGETS root as empty history");
    }

    peer.send(":server 005 " + identity.nick + " MSGREFTYPES=msgid,timestamp :supported");
    waitEvent(client, events, 0, [](const Event &e) {
        return e.type == "network" && tag(e, "MSGREFTYPES") == "msgid,timestamp";
    });
    client.history({.target = "#retry",
                    .direction = HistoryDirection::Before,
                    .first = {"original-id", time},
                    .requestId = "retry"});
    peer.expect("CHATHISTORY", {"BEFORE", "#retry", "msgid=original-id", "50"});
    peer.send(prefix("retry") +
              ":server FAIL CHATHISTORY INVALID_MSGREFTYPE #retry :use timestamp");
    peer.expect("CHATHISTORY", {"BEFORE", "#retry", "timestamp=" + time, "50"});
    collect(client, events);
    require(
        std::none_of(events.begin(), events.end(),
                     [](const Event &e) { return e.type == "history_end" && e.label == "retry"; }),
        "Recoverable msgid failure prematurely ended the logical request");
    peer.send(prefix("retry") +
              ":server FAIL CHATHISTORY INVALID_MSGREFTYPE #retry :timestamp denied");
    const auto retryEnd = end("retry");
    require(tag(retryEnd, "error") == "timestamp denied" &&
                tag(retryEnd, "failure-code") == "INVALID_MSGREFTYPE" &&
                tag(retryEnd, "ref-type") == "timestamp",
            "Timestamp fallback lost explicit failure or selector metadata");
    peer.fence();
    peer.quietUntil(Clock::now() + 100ms);
    for (const string cap : {"invalid", "0", "17"}) {
        peer.send(":server 005 " + identity.nick + " CHATHISTORY=" + cap + " :supported");
        waitEvent(client, events, 0, [&](const Event &e) {
            return e.type == "network" && tag(e, "CHATHISTORY") == cap;
        });
        const string id = "cap-" + cap;
        client.history({.direction = HistoryDirection::Targets,
                        .first = {"", "2026-11-02T00:00:00Z"},
                        .second = {"", "2026-10-01T00:00:00Z"},
                        .requestId = id,
                        .limit = 100});
        peer.expect("CHATHISTORY",
                    {"TARGETS", "timestamp=2026-11-02T00:00:00.000Z",
                     "timestamp=2026-10-01T00:00:00.000Z", cap == "17" ? "17" : "100"});
        peer.send(prefix(id) + ":server BATCH +cap draft/chathistory-targets\r\n"
                               ":server BATCH -cap");
        require(tag(end(id), "error").empty(), "Advertised history cap broke TARGETS completion");
    }
    peer.send(":server 005 " + identity.nick + " CHATHISTORY=0 :supported");
    peer.fence();
    const int slots = labeled ? 6 : 1;
    const auto timeoutStart = Clock::now();
    for (int i = 0; i < slots; ++i)
        latest("timeout-" + std::to_string(i));
    client.history({.target = "#bounds", .requestId = "timeout-queued"});
    client.command("PING :timeout-admitted");
    peer.expect("PING", {"timeout-admitted"});
    peer.quietUntil(Clock::now() + 100ms);
    const auto deadline = timeoutStart + 40s;
    for (;;) {
        collect(client, events);
        const auto completed = std::count_if(events.begin(), events.end(), [](const Event &e) {
            return e.type == "history_end" && e.label.starts_with("timeout-");
        });
        if (completed == slots + 1)
            break;
        require(Clock::now() < deadline, "History timeout excluded time waiting in the queue");
        std::this_thread::sleep_for(10ms);
    }
    require(Clock::now() >= timeoutStart + 34s,
            "History requests expired before their 35s deadline");
    for (const auto &e : events)
        if (e.type == "history_end" && e.label.starts_with("timeout-"))
            require(tag(e, "timeout") == "1" && !tag(e, "error").empty() &&
                        tag(e, "count") == "0" && tag(e, "end").empty(),
                    "Active or queued timeout fabricated a successful empty page");
    client.stop();
    collect(client, events);
}

void historyQueueChecks(SSL_CTX *context, const Identity &identity, bool labeled) {
    WirePeer peer(context);
    NetworkClient client(server(peer.port), identity);
    vector<Event> events;
    registerClient(peer, client, events, identity,
                   "message-tags batch draft/chathistory soju.im/search draft/channel-rename" +
                       string(labeled ? " labeled-response" : ""));
    auto admitted = [&] {
        client.command("PING :history-admitted");
        peer.expect("PING", {"history-admitted"});
    };
    auto request = [&](const string &target, const string &id, bool initial) {
        client.history({.target = target, .requestId = id, .initial = initial});
    };
    auto wire = [&](const string &target, const string &id) {
        const auto message = peer.expect("CHATHISTORY", {"LATEST", target, "*", "50"});
        require(labeled ? message.tags.contains("label") && message.tags.at("label") == id
                        : !message.tags.contains("label"),
                "History wire label lost caller correlation");
        const auto index = waitEvent(client, events, 0, [&](const Event &e) {
            return e.type == "history_begin" && e.label == id;
        });
        require(events[index].target == target, "History begin lost target");
    };
    unsigned batch = 0;
    auto finish = [&](const string &target, const string &id) {
        const string name = "history-check-" + std::to_string(++batch);
        peer.send(string(labeled ? "@label=" + id + " " : "") + ":server BATCH +" + name +
                  " chathistory " + target + "\r\n:server BATCH -" + name);
        const auto index = waitEvent(client, events, 0, [&](const Event &e) {
            return e.type == "history_end" && e.label == id;
        });
        require(tag(events[index], "error").empty() && tag(events[index], "count") == "0",
                "Empty history batch did not complete normally");
    };
    // AROUND uses the server's advertised reference types and page limit, while
    // retaining the same request correlation on labeled and unlabeled servers.
    peer.send(":server 005 " + identity.nick + " CHATHISTORY=17 MSGREFTYPES=timestamp :supported");
    waitEvent(client, events, 0, [](const Event &e) {
        return e.type == "network" && tag(e, "kind") == "isupport" &&
               tag(e, "MSGREFTYPES") == "timestamp";
    });
    client.history({.target = "#around",
                    .direction = HistoryDirection::Around,
                    .first = {"selected-id", "2026-11-01T12:00:00Z"},
                    .requestId = "around-time"});
    const auto around = peer.expect(
        "CHATHISTORY", {"AROUND", "#around", "timestamp=2026-11-01T12:00:00.000Z", "17"});
    require(labeled ? around.tags.contains("label") && around.tags.at("label") == "around-time"
                    : !around.tags.contains("label"),
            "AROUND wire label lost caller correlation");
    const auto aroundBegin = waitEvent(client, events, 0, [](const Event &e) {
        return e.type == "history_begin" && e.label == "around-time";
    });
    require(events[aroundBegin].target == "#around" &&
                tag(events[aroundBegin], "direction") == "around" &&
                tag(events[aroundBegin], "initial").empty(),
            "AROUND begin lost foreground routing");
    peer.send(string(labeled ? "@label=around-time " : "") +
              ":server BATCH +around-check chathistory #around\r\n"
              "@batch=around-check;msgid=selected-id :Peer!u@h PRIVMSG #around :selected\r\n"
              ":server BATCH -around-check");
    const auto aroundEnd = waitEvent(client, events, 0, [](const Event &e) {
        return e.type == "history_end" && e.label == "around-time";
    });
    require(tag(events[aroundEnd], "direction") == "around" &&
                tag(events[aroundEnd], "count") == "1" && tag(events[aroundEnd], "error").empty(),
            "AROUND completion lost direction or result count");
    peer.send(":server 005 " + identity.nick + " CHATHISTORY=200 MSGREFTYPES=msgid :supported");
    waitEvent(client, events, 0, [](const Event &e) {
        return e.type == "network" && tag(e, "kind") == "isupport" &&
               tag(e, "MSGREFTYPES") == "msgid";
    });
    client.history({.target = "#around",
                    .direction = HistoryDirection::Around,
                    .first = {"selected-id", "2026-11-01T12:00:00Z"},
                    .requestId = "around-id"});
    peer.expect("CHATHISTORY", {"AROUND", "#around", "msgid=selected-id", "50"});
    peer.send(string(labeled ? "@label=around-id " : "") +
              ":server FAIL CHATHISTORY INVALID_PARAMS #around :history denied");
    const auto aroundFailure = waitEvent(client, events, 0, [](const Event &e) {
        return e.type == "history_end" && e.label == "around-id";
    });
    require(tag(events[aroundFailure], "direction") == "around" &&
                tag(events[aroundFailure], "error") == "history denied",
            "AROUND failure lost correlation");
    client.history({.target = "#around",
                    .direction = HistoryDirection::Around,
                    .first = {"", "2026-11-01T12:00:00Z"},
                    .requestId = "around-invalid"});
    const auto aroundInvalid = waitEvent(client, events, 0, [](const Event &e) {
        return e.type == "history_end" && e.label == "around-invalid";
    });
    require(tag(events[aroundInvalid], "direction") == "around" &&
                tag(events[aroundInvalid], "rejected") == "1" &&
                !tag(events[aroundInvalid], "error").empty(),
            "Unsupported AROUND selector was not rejected");
    // Fill active slots so ranking changes operate only on pending work.
    const int active = labeled ? 6 : 1;
    for (int i = 0; i < active; ++i) {
        const string id = "held-" + std::to_string(i);
        request("#" + id, id, true);
        wire("#" + id, id);
    }
    request("#a", "initial-a", true);
    request("#b", "initial-b", true);
    request("#c", "initial-c", true);
    request("#foreground", "foreground", false);
    client.command("SEARCH #search needle");
    request("#cancel", "canceled", true);
    client.cancelInitialHistory("#foreground");
    client.cancelInitialHistory("#cancel");
    client.prioritizeHistory({"#c", "#a", "#b"});
    admitted();
    peer.quietUntil(Clock::now() + 100ms);
    const auto canceled = waitEvent(client, events, 0, [](const Event &e) {
        return e.type == "history_end" && e.label == "canceled";
    });
    require(tag(events[canceled], "initial") == "1" && tag(events[canceled], "cancelled") == "1" &&
                !tag(events[canceled], "error").empty(),
            "Queued cancellation did not retire its initial request");
    finish("#held-0", "held-0");
    wire("#c", "initial-c");
    client.cancelInitialHistory("#c");
    client.prioritizeHistory({"#b", "#a", "#c"});
    admitted();
    peer.send(":server RENAME #b #renamed-b\r\n:server RENAME #c #renamed-c");
    peer.fence();
    finish("#renamed-c", "initial-c");
    wire("#renamed-b", "initial-b");
    finish("#renamed-b", "initial-b");
    wire("#foreground", "foreground");
    const auto foregroundBegin = waitEvent(client, events, 0, [](const Event &e) {
        return e.type == "history_begin" && e.label == "foreground";
    });
    require(tag(events[foregroundBegin], "initial").empty(),
            "Foreground history was marked initial");
    peer.send(string(labeled ? "@label=foreground " : "") +
              ":server FAIL CHATHISTORY INVALID_PARAMS #foreground :history denied");
    const auto denied = waitEvent(client, events, 0, [](const Event &e) {
        return e.type == "history_end" && e.label == "foreground";
    });
    require(tag(events[denied], "error") == "history denied" &&
                tag(events[denied], "initial").empty() && tag(events[denied], "rejected").empty(),
            "Protocol history failure lost correlation or retry semantics");
    const auto search = peer.expect("SEARCH", {"in=#search;text=needle;limit=100"});
    const auto searchBegin = waitEvent(client, events, 0, [](const Event &e) {
        return e.type == "history_begin" && e.target == "#search";
    });
    const string searchId = events[searchBegin].label;
    require(!searchId.empty() && tag(events[searchBegin], "initial").empty() &&
                tag(events[searchBegin], "direction") == "search",
            "Generic search did not stay foreground");
    require(!labeled || search.tags.at("label") == searchId, "Search label mismatch");
    peer.send(string(labeled ? "@label=" + searchId + " " : "") +
              ":server BATCH +search-result soju.im/search\r\n:server BATCH -search-result");
    const auto searchEnd = waitEvent(client, events, 0, [&](const Event &e) {
        return e.type == "history_end" && e.label == searchId;
    });
    require(tag(events[searchEnd], "error").empty() &&
                tag(events[searchEnd], "direction") == "search" &&
                tag(events[searchEnd], "count") == "0",
            "Targetless native Soju SEARCH root did not complete normally");
    wire("#a", "initial-a");
    // Hold the last request while exercising the 128-pending bound.
    client.history({.target = "invalid target",
                    .direction = HistoryDirection::After,
                    .first = {"cursor", ""},
                    .requestId = "invalid-initial",
                    .initial = true});
    client.history({.target = "#invalid",
                    .direction = HistoryDirection::Before,
                    .first = {"bad cursor", ""},
                    .requestId = "invalid-foreground"});
    for (int i = 0; i < 128; ++i)
        request("#queued-" + std::to_string(i), "queued-" + std::to_string(i), true);
    request("#overflow", "overflow-initial", true);
    request("#overflow", "overflow-foreground", false);
    admitted();
    for (const string id :
         {"invalid-initial", "invalid-foreground", "overflow-initial", "overflow-foreground"}) {
        const auto index = waitEvent(client, events, 0, [&](const Event &e) {
            return e.type == "history_end" && e.label == id;
        });
        const string direction = id == "invalid-initial"      ? "after"
                                 : id == "invalid-foreground" ? "before"
                                                              : "latest";
        require(!tag(events[index], "error").empty() && tag(events[index], "rejected") == "1" &&
                    (tag(events[index], "initial") == "1") == id.ends_with("-initial") &&
                    tag(events[index], "direction") == direction,
                "Rejected history lost request metadata");
    }
    client.stop();
    bool stoppedRejected = false;
    try {
        client.history({.target = "#stopped",
                        .direction = HistoryDirection::After,
                        .first = {"cursor", ""},
                        .requestId = "stopped-initial",
                        .initial = true});
    } catch (const std::runtime_error &) {
        stoppedRejected = true;
    }
    require(stoppedRejected, "Stopped-client history enqueue did not throw");
    collect(client, events);
    require(std::none_of(events.begin(), events.end(),
                         [](const Event &e) { return e.label == "stopped-initial"; }),
            "Unadmitted history request emitted a duplicate terminal event");
    for (int i = 0; i < 128; ++i) {
        const string id = "queued-" + std::to_string(i);
        require(std::count_if(events.begin(), events.end(),
                              [&](const Event &e) {
                                  return e.type == "history_end" && e.label == id &&
                                         e.target == "#" + id && tag(e, "initial") == "1" &&
                                         tag(e, "direction") == "latest" &&
                                         !tag(e, "error").empty();
                              }) == 1,
                "Disconnect did not retire queued history exactly once");
    }
    require(std::count_if(events.begin(), events.end(),
                          [](const Event &e) {
                              return e.type == "history_end" && e.label == "initial-a" &&
                                     !tag(e, "error").empty() && tag(e, "initial") == "1";
                          }) == 1,
            "Disconnect did not retire active initial history");
    require(std::none_of(events.begin(), events.end(),
                         [](const Event &e) {
                             return e.type == "error" &&
                                    (e.label.starts_with("invalid-") ||
                                     e.label.starts_with("overflow-") || e.label == "canceled" ||
                                     e.label == "foreground");
                         }),
            "History rejection emitted duplicate generic diagnostics");
}

void playbackChecks(SSL_CTX *context, const Identity &identity) {
    WirePeer peer(context);
    NetworkClient client(server(peer.port, "", "znc"), identity);
    vector<Event> events;
    registerClient(peer, client, events, identity,
                   "message-tags batch server-time znc.in/playback");
    peer.send("@znc.in/playback;msgid=standalone-playback;history-owner=spoof "
              ":alice!user@localhost PRIVMSG #playback :genuine standalone playback");
    const auto standalone = waitEvent(client, events, 0, [](const Event &e) {
        return e.type == "message" && e.id == "standalone-playback";
    });
    require(events[standalone].history && tag(events[standalone], "history-standalone") == "1" &&
                !events[standalone].tags.contains("history-owner"),
            "Genuine standalone ZNC playback lacked trusted standalone classification");
    peer.send("@history-standalone=1;msgid=forged-playback "
              ":alice!user@localhost PRIVMSG #playback :ordinary ZNC live message");
    const auto forged = waitEvent(client, events, 0, [](const Event &e) {
        return e.type == "message" && e.id == "forged-playback";
    });
    require(!events[forged].history && !events[forged].tags.contains("history-standalone"),
            "Arbitrary ZNC wire marker became trusted standalone playback");
    client.history({.target = "#playback",
                    .direction = HistoryDirection::After,
                    .first = {"", "2026-11-05T00:00:00.123Z"},
                    .requestId = "playback-after",
                    .initial = true});
    peer.expect("ZNC", {"*playback", "PLAY #playback 1793836800.123"});
    peer.send(":server BATCH +playback znc.in/playback #playback");
    peer.send("@batch=playback;time=2026-11-05T00:00:01.250Z;msgid=playback-message "
              ":alice!user@localhost PRIVMSG #playback :actual ZNC history");
    peer.send(":server BATCH -playback");
    const auto &end = events[waitEvent(client, events, 0, [](const Event &event) {
        return event.type == "history_end" && event.target == "#playback";
    })];
    const string historyLabel = end.label;
    require(historyLabel == "playback-after" && tag(end, "initial") == "1" &&
                tag(end, "error").empty() && tag(end, "count") == "1" &&
                tag(end, "direction") == "after" && tag(end, "batch") == "playback",
            "ZNC playback did not complete its history request");
    require(std::any_of(events.begin(), events.end(),
                        [&](const Event &event) {
                            return event.type == "history_begin" && event.target == "#playback" &&
                                   event.label == historyLabel;
                        }),
            "ZNC playback omitted history_begin");
    require(std::any_of(events.begin(), events.end(),
                        [&](const Event &event) {
                            return event.type == "message" && event.target == "#playback" &&
                                   event.history && event.label == historyLabel &&
                                   event.id == "playback-message" &&
                                   event.text == "actual ZNC history" &&
                                   event.time == "2026-11-05T00:00:01.250Z" &&
                                   event.timestamp == 1793836801;
                        }),
            "Real ZNC batch message was not delivered as historical");
    const auto from = events.size();
    client.history({.target = "#missing", .requestId = "playback-missing"});
    peer.expect("ZNC", {"*playback", "PLAY #missing 0"});
    peer.send(":server 401 " + identity.nick + " *playback :No such nick");
    const auto &rejected = events[waitEvent(client, events, from, [](const Event &event) {
        return event.type == "history_end" && event.target == "#missing";
    })];
    require(!rejected.label.empty() && tag(rejected, "count") == "0" &&
                tag(rejected, "error") == "ZNC playback module is not available",
            "Missing ZNC playback module did not produce an explicit history error");
    peer.send(":server CAP " + identity.nick + " DEL :znc.in/playback");
    peer.fence();
    const auto unsupportedFrom = events.size();
    client.history(
        {.target = "#unsupported", .requestId = "playback-unsupported", .initial = true});
    const auto &unsupported =
        events[waitEvent(client, events, unsupportedFrom, [](const Event &event) {
            return event.type == "history_end" && event.target == "#unsupported";
        })];
    require(unsupported.label == "playback-unsupported" && tag(unsupported, "initial") == "1" &&
                tag(unsupported, "rejected") == "1" &&
                tag(unsupported, "error") == "Server does not advertise history playback",
            "ZNC without the native playback capability was treated as supported");
    peer.fence(); // No module command may precede this PONG without the capability.
    client.stop();
    collect(client, events);
}
void sojuChecks(SSL_CTX *context, const Identity &identity) {
    const string caps = "sasl=PLAIN batch soju.im/bouncer-networks "
                        "soju.im/bouncer-networks-notify labeled-response";
    for (const string code : {"INVALID_NETID", "ACCOUNT_REQUIRED"}) {
        const bool sasl = code != "ACCOUNT_REQUIRED";
        WirePeer rejectedPeer(context);
        NetworkClient rejected(server(rejectedPeer.port, sasl ? "PLAIN" : "", "soju:missing"),
                               identity);
        vector<Event> rejectedEvents;
        registerClient(rejectedPeer, rejected, rejectedEvents, identity,
                       "sasl=PLAIN batch soju.im/bouncer-networks", sasl, false, {}, true,
                       "missing", false, code);
    }
    {
        WirePeer peer(context);
        NetworkClient bound(server(peer.port, "PLAIN", "soju:42"), identity);
        vector<Event> events;
        registerClient(peer, bound, events, identity, "sasl=PLAIN batch soju.im/bouncer-networks",
                       true, false, {}, true, "42");
        peer.fence(); // A bound standalone client must not request LISTNETWORKS.
        bound.sojuMutation("DELNETWORK", "42", {});
        waitEvent(bound, events, 0, [](const Event &e) {
            return tag(e, "kind") == "bouncer-fail" && tag(e, "command") == "DELNETWORK";
        });
        peer.fence(); // Management cannot run on a bound session.
        bound.stop();
    }
    {
        WirePeer unsupportedPeer(context);
        auto unsupported = server(unsupportedPeer.port, "PLAIN", "soju");
        unsupported.sojuAccount = true;
        NetworkClient unavailable(unsupported, identity);
        vector<Event> unavailableEvents;
        registerClient(unsupportedPeer, unavailable, unavailableEvents, identity, "sasl=PLAIN",
                       true);
        unavailable.sojuMutation("DELNETWORK", "42", {});
        waitEvent(unavailable, unavailableEvents, 0, [](const Event &e) {
            return tag(e, "kind") == "bouncer-fail" && tag(e, "command") == "DELNETWORK";
        });
        unsupportedPeer.fence(); // Ready alone cannot authorize unsupported management.
        unavailable.stop();
    }
    WirePeer peer(context);
    auto config = server(peer.port, "PLAIN", "soju");
    config.sojuAccount = true;
    NetworkClient client(config, identity);
    client.sojuMutation("DELNETWORK", "not-ready", {});
    vector<Event> events;
    registerClient(peer, client, events, identity, caps, true, false, {}, true, "", true);
    WirePeer childPeerOne(context), childPeerTwo(context);
    auto childOneConfig = server(childPeerOne.port, "PLAIN", "soju:42");
    auto childTwoConfig = server(childPeerTwo.port, "PLAIN", "soju:43");
    childOneConfig.name = "child-one";
    childTwoConfig.name = "child-two";
    NetworkClient childOne(childOneConfig, identity), childTwo(childTwoConfig, identity);
    vector<Event> childOneEvents, childTwoEvents;
    const string childCaps = "sasl=PLAIN batch soju.im/bouncer-networks";
    registerClient(childPeerOne, childOne, childOneEvents, identity, childCaps, true, false, {},
                   true, "42");
    registerClient(childPeerTwo, childTwo, childTwoEvents, identity, childCaps, true, false, {},
                   true, "43");
    childPeerOne.send(":alice!u@h PRIVMSG #one :first child only");
    childPeerTwo.send(":bob!u@h PRIVMSG #two :second child only");
    const auto one = waitEvent(childOne, childOneEvents, 0, [](const Event &e) {
        return e.type == "message" && e.text == "first child only";
    });
    const auto two = waitEvent(childTwo, childTwoEvents, 0, [](const Event &e) {
        return e.type == "message" && e.text == "second child only";
    });
    require(childOneEvents[one].server == "child-one" && childTwoEvents[two].server == "child-two",
            "Concurrent bound sessions must retain separate source identities");
    childOne.stop();
    childPeerTwo.fence();
    peer.fence(); // Stopping one child leaves both sibling and root sockets alive.
    require(std::any_of(events.begin(), events.end(),
                        [](const Event &e) {
                            return tag(e, "kind") == "bouncer-fail" && e.id == "not-ready" &&
                                   tag(e, "command") == "DELNETWORK";
                        }),
            "Typed management must fail rather than transmit before Ready");
    require(std::any_of(events.begin(), events.end(),
                        [](const Event &e) {
                            return tag(e, "kind") == "authentication" &&
                                   tag(e, "authenticated") == "1" && tag(e, "mechanism") == "PLAIN";
                        }),
            "SASL success must publish actual authentication");
    const size_t emptyFrom = events.size();
    peer.send(":server BATCH +empty soju.im/bouncer-networks");
    peer.send(":server BATCH -empty");
    const auto emptyEnd = waitEvent(client, events, emptyFrom, [](const Event &e) {
        return tag(e, "kind") == "bouncer-list" && tag(e, "phase") == "end";
    });
    require(emptyEnd > emptyFrom && events[emptyEnd].id == "empty" &&
                tag(events[emptyEnd - 1], "phase") == "begin",
            "Empty catalog must deliver begin then completion");
    peer.send(":server BATCH +list soju.im/bouncer-networks");
    peer.send("@batch=list :server BOUNCER NETWORK 42 :name=Hello\\sWorld;host=irc.test");
    peer.send(":server BATCH -list");
    peer.send(":server BOUNCER NETWORK 42 :state=connected;name=");
    peer.send(":server BOUNCER NETWORK 42 *");
    const size_t deleted = waitEvent(client, events, emptyEnd + 1, [](const Event &e) {
        return tag(e, "kind") == "bouncer" && tag(e, "deleted") == "1";
    });
    require(events[deleted - 1].tags.contains("name") && tag(events[deleted - 1], "name").empty() &&
                !events[deleted - 1].tags.contains("host"),
            "Partial catalog update must preserve explicit removal and absent fields");
    require(events[deleted - 3].text == "Hello World" && tag(events[deleted - 2], "phase") == "end",
            "Catalog attributes and batch completion lost ordering");
    const string secret = "wire-secret ;\\ value";
    client.sojuMutation("ADDNETWORK", "", {{"host", "irc.test"}, {"pass", secret}});
    const auto add = peer.read(Clock::now() + 3s);
    require(add.command == "BOUNCER" &&
                add.params ==
                    vector<string>{"ADDNETWORK", "host=irc.test;pass=" + irc::escapeTag(secret)},
            "ADDNETWORK must escape attribute values, not inject extra attributes");
    const auto addLabel = add.tags.at("label");
    const size_t staleFrom = events.size();
    peer.send("@label=stale :server BOUNCER ADDNETWORK 98");
    peer.send("@label=stale :server FAIL BOUNCER UNKNOWN_ATTRIBUTE ADDNETWORK pass :" + secret);
    peer.fence();
    collect(client, events);
    require(std::none_of(events.begin() + staleFrom, events.end(),
                         [](const Event &e) {
                             return tag(e, "kind") == "bouncer-ack" ||
                                    tag(e, "kind") == "bouncer-fail";
                         }),
            "Stale labeled acknowledgement/failure must not complete a pending mutation");
    const size_t listFailureFrom = events.size();
    peer.send(":server FAIL BOUNCER LIST_FAILED LISTNETWORKS :catalog unavailable");
    const auto listFailure = waitEvent(client, events, listFailureFrom, [](const Event &e) {
        return tag(e, "kind") == "bouncer-fail" && tag(e, "command") == "LISTNETWORKS";
    });
    require(events[listFailure].params.empty(),
            "Independent unlabeled catalog failure must remain safe during labeled mutation");
    peer.send("@label=" + addLabel + " :server BOUNCER ADDNETWORK 99");
    waitEvent(client, events, deleted + 1, [](const Event &e) {
        return tag(e, "kind") == "bouncer-ack" && tag(e, "command") == "ADDNETWORK" &&
               e.id == "99" && e.params.empty();
    });
    client.sojuMutation("CHANGENETWORK", "99", {{"realname", "Two ; words\\"}});
    const auto change = peer.expect(
        "BOUNCER", {"CHANGENETWORK", "99", "realname=" + irc::escapeTag("Two ; words\\")});
    peer.send("@label=" + change.tags.at("label") +
              " :server FAIL BOUNCER UNKNOWN_ATTRIBUTE CHANGENETWORK pass :" + secret);
    const size_t failed = waitEvent(client, events, deleted + 1, [](const Event &e) {
        return tag(e, "kind") == "bouncer-fail" && tag(e, "command") == "CHANGENETWORK" &&
               e.id == "99";
    });
    require(events[failed].params.empty() &&
                events[failed].text.find("wire-secret") == string::npos,
            "FAIL must not expose echoed credentials");
    client.sojuMutation("DELNETWORK", "99", {});
    const auto removal = peer.expect("BOUNCER", {"DELNETWORK", "99"});
    peer.send("@label=" + removal.tags.at("label") + " :server BOUNCER DELNETWORK 99");
    waitEvent(client, events, failed + 1, [](const Event &e) {
        return tag(e, "kind") == "bouncer-ack" && tag(e, "command") == "DELNETWORK";
    });
    const size_t rawFrom = events.size();
    peer.send(":server BOUNCER UNKNOWN :pass=" + secret);
    const size_t raw = waitEvent(client, events, rawFrom, [](const Event &e) {
        return tag(e, "command") == "BOUNCER" && tag(e, "kind") == "raw";
    });
    require(events[raw].params.empty() && events[raw].text.find("wire-secret") == string::npos,
            "Unknown BOUNCER reply must redact raw attributes");
    for (const auto &[key, value] :
         std::map<string, string>{{"bad;pass", "value"}, {"name", "bad\r\nQUIT"}}) {
        bool rejected = false;
        try {
            client.sojuMutation("CHANGENETWORK", "99", {{key, value}});
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        require(rejected, "Mutation must reject attribute-key and framing injection");
    }
    peer.send(":server 005 " + identity.nick + " soju.im/FILEHOST=https://files.test/uploads :ok");
    const size_t advertised = waitEvent(client, events, raw + 1, [](const Event &e) {
        return tag(e, "kind") == "isupport" &&
               tag(e, "soju.im/FILEHOST") == "https://files.test/uploads";
    });
    peer.send(":server 005 " + identity.nick + " -soju.im/FILEHOST :ok");
    waitEvent(client, events, advertised + 1, [](const Event &e) {
        return tag(e, "kind") == "isupport" && !e.tags.contains("soju.im/FILEHOST");
    });
    peer.fence();
    client.stop();
    childTwo.stop();
}
void commandCorrelationChecks(SSL_CTX *context, const Identity &identity, bool labeled) {
    WirePeer peer(context);
    NetworkClient client(server(peer.port, labeled ? "PLAIN" : "", labeled ? "soju:42" : "none"),
                         identity);
    vector<Event> events;
    // Negotiate one cap even without labeled-response: the registration helper
    // checks the actual ACK snapshot, which an empty CAP LS never produces.
    registerClient(peer, client, events, identity,
                   labeled
                       ? "sasl=PLAIN message-tags batch labeled-response soju.im/bouncer-networks"
                       : "message-tags",
                   labeled, false, {}, true, labeled ? "42" : "");
    auto request = [&](const string &line, const string &id) {
        client.command(line, "", id);
        auto message = peer.read();
        require(message.command == line.substr(0, line.find(' ')),
                "Correlated request changed the outbound command");
        const auto found = message.tags.find("label");
        require((found != message.tags.end()) == labeled, "Command label CAP gate failed");
        if (found != message.tags.end()) {
            require(found->second != id, "Opaque command ID was used as a wire label");
            return found->second;
        }
        return string{};
    };
    auto response = [&](const string &numeric, size_t from = 0) {
        return waitEvent(client, events, from,
                         [&](const Event &e) { return tag(e, "command") == numeric; });
    };
    auto complete = [&](const string &id, size_t from = 0) {
        return waitEvent(client, events, from, [&](const Event &e) {
            return tag(e, "command-id") == id && tag(e, "command-complete") == "1";
        });
    };
    const string recipient = identity.nick;
    if (labeled) {
        auto label = request("WHOIS Alice", "opaque ; user/id");
        peer.send("@label=" + label + " :server 318 " + recipient + " Alice :End of WHOIS");
        auto index = complete("opaque ; user/id");
        require(events[index].label == label &&
                    tag(events[index], "command-response") == "318 Alice End of WHOIS",
                "Direct labeled response lost its original parameters or command ID");
        require(tag(events[index], "command-name") == "WHOIS",
                "Labeled WHOIS lost its structured response family");

        const auto first = request("WHOIS Bob", "nested");
        const auto second = request("WHOIS Carol", "interleaved");
        peer.send("@label=" + first + " :server BATCH +outer labeled-response");
        peer.send("@batch=outer :server BATCH +inner example/child");
        peer.send("@batch=inner :server 311 " + recipient + " Bob user host * :Bob Realname");
        peer.send("@label=" + second + " :server 318 " + recipient + " Carol :done");
        index = response("311", index + 1);
        require(tag(events[index], "command-id") == "nested" &&
                    tag(events[index], "command-complete").empty(),
                "Nested labeled batch did not retain correlation until outer close");
        complete("interleaved", index + 1);
        peer.send(":server BATCH -inner");
        peer.fence();
        collect(client, events);
        require(std::none_of(events.begin(), events.end(),
                             [](const Event &e) {
                                 return tag(e, "command-id") == "nested" &&
                                        tag(e, "command-complete") == "1";
                             }),
                "Inner batch prematurely completed outer command");
        peer.send(":server BATCH -outer");
        index = complete("nested");
        require(tag(events[index], "kind") == "command-complete" && events[index].text.empty() &&
                    !events[index].tags.contains("command-response"),
                "Batch close emitted visible output");
        require(tag(events[index], "command-name") == "WHOIS",
                "Silent completion lost its WHOIS family");

        label = request("WHOIS LateChild", "late-child");
        peer.send("@label=" + label + " :server BATCH +early-parent labeled-response");
        peer.send("@batch=early-parent :server BATCH +late-member example/child");
        peer.send(":server BATCH -early-parent");
        peer.fence();
        collect(client, events);
        require(std::none_of(events.begin(), events.end(),
                             [](const Event &e) {
                                 return tag(e, "command-id") == "late-child" &&
                                        tag(e, "command-complete") == "1";
                             }),
                "Parent closure ignored an outstanding differently typed member");
        peer.send("@batch=late-member :server 318 " + recipient + " LateChild :done");
        index = response("318", index + 1);
        require(tag(events[index], "command-id") == "late-child",
                "Outstanding child lost its inherited label");
        peer.send(":server BATCH -late-member");
        complete("late-child", index + 1);

        label = request("MODE #quiet +m", "ack");
        peer.send("@label=" + label + " :server ACK");
        index = complete("ack", index + 1);
        require(tag(events[index], "kind") == "command-complete" && events[index].text.empty(),
                "ACK did not produce a silent completion");

        label = request("WHOIS Missing", "labeled-error");
        peer.send("@label=" + label + " :server 401 " + recipient + " Missing :No such nick");
        index = complete("labeled-error", index + 1);
        require(tag(events[index], "command-response") == "401 Missing No such nick",
                "Labeled numeric error lost response context");

        label = request("HELP", "standard-error");
        peer.send("@label=" + label + " :server FAIL HELP INVALID :No help available");
        index = complete("standard-error", index + 1);
        require(tag(events[index], "kind") == "standard-reply",
                "Labeled standard reply lost its normal event semantics");

        label = request("BOUNCER CHANGENETWORK 42 password=never-echo-this", "bouncer-error");
        peer.send("@label=" + label +
                  " :server FAIL BOUNCER INVALID_ATTRS CHANGENETWORK 42 :never-echo-this");
        index = complete("bouncer-error", index + 1);
        require(tag(events[index], "kind") == "standard-reply" && events[index].params.empty() &&
                    tag(events[index], "command-response").find("never-echo-this") ==
                        std::string_view::npos &&
                    events[index].text.find("never-echo-this") == string::npos,
                "Bouncer rejection lost correlation or exposed credential attributes");

        label = request("NICK Occupied", "nick-error");
        peer.send("@label=" + label + " :server 433 " + recipient + " Occupied :Nickname in use");
        index = complete("nick-error", index + 1);
        require(tag(events[index], "command-response") == "433 Occupied Nickname in use",
                "Early nickname rejection bypassed command correlation");
        label = request("PING user-token", "ping");
        peer.send("@label=" + label + " :server PONG server :user-token");
        index = complete("ping", index + 1);
        require(tag(events[index], "command-response") == "PONG server user-token",
                "Early PONG dispatch bypassed command correlation");
        label = request("METADATA #quiet GET avatar", "metadata-retry");
        peer.send("@label=" + label + " :server 774 " + recipient + " #quiet 5");
        index = complete("metadata-retry", index + 1);
        require(tag(events[index], "command-response") == "774 #quiet 5",
                "Metadata retry processing discarded a correlated reply");

        label = request("WHOIS Dave", "chat-is-not-reply");
        peer.send("@label=" + label + ";command-name=WHOIS;command-id=forged :Dave!u@h PRIVMSG " +
                  recipient + " :ordinary chat");
        index = waitEvent(client, events, index + 1, [](const Event &e) {
            return e.type == "message" && e.text == "ordinary chat";
        });
        require(tag(events[index], "command-id").empty(), "Ordinary chat acquired a command route");
        require(tag(events[index], "command-name").empty(),
                "Incoming tags forged a trusted command family");
        peer.send("@label=" + label + " :server ACK");
        complete("chat-is-not-reply", index + 1);
    } else {
        request("WHOIS Al[ce", "whois");
        peer.send(":server 311 " + recipient + " al{ce user host * :Alice Realname");
        auto index = response("311");
        require(tag(events[index], "command-id") == "whois" &&
                    tag(events[index], "command-complete").empty(),
                "Unlabeled WHOIS did not use the IRC casemap");
        peer.send(":server 301 " + recipient + " al{ce :Away");
        index = response("301", index + 1);
        require(events[index].type == "away" && events[index].nick == "al{ce" &&
                    tag(events[index], "command-id") == "whois",
                "WHOIS away response changed state semantics");
        peer.send(":server 318 " + recipient + " al{ce :done");
        complete("whois", index + 1);

        request("WHO #who", "who");
        peer.send(":server 352 " + recipient + " #who u h server Person H :0 Real Name");
        index = response("352");
        require(events[index].type == "away" && events[index].target == "#who" &&
                    tag(events[index], "command-id") == "who",
                "WHO response changed state target or lost correlation");
        peer.send(":server 315 " + recipient + " #who :done");
        complete("who", index + 1);

        request("NAMES #names", "names");
        peer.send(":server 353 " + recipient + " = #names :@One +Two");
        index = response("353");
        require(events[index].type == "names" && events[index].target == "#names" &&
                    events[index].params == vector<string>({"@One", "+Two"}) &&
                    tag(events[index], "command-response") == "353 = #names @One +Two",
                "NAMES did not preserve both state params and original response");
        peer.send(":server 366 " + recipient + " #names :done");
        index = complete("names", index + 1);
        require(events[index].type == "names" && tag(events[index], "complete") == "1",
                "NAMES terminal no longer updates membership");

        request("LIST", "list");
        peer.send(":server 322 " + recipient + " #listed 42 :Channel topic");
        index = response("322");
        require(tag(events[index], "command-id") == "list", "LIST row was not correlated");
        peer.send(":server 323 " + recipient + " :done");
        complete("list", index + 1);
        // Repeat sequential same-target exchanges: consumed optional evidence is resolved.
        for (const string command : {"MODE", "TOPIC", "MODE", "TOPIC"}) {
            const bool mode = command == "MODE";
            const string primary = mode ? "324" : "332", optional = mode ? "329" : "333";
            const string target = mode ? "#mode-normal" : "#topic-normal";
            const string id = mode ? "mode-normal" : "topic-normal";
            request(command + " " + target, id);
            peer.send(":server " + primary + " " + recipient + " " + target +
                      (mode ? " +nt" : " :Normal topic"));
            index = response(primary, index + 1);
            require(tag(events[index], "command-id") == id &&
                        tag(events[index], "command-complete").empty(),
                    "Primary reply lost its origin before supplemental metadata");
            peer.send(":server " + optional + " " + recipient + " " + target +
                      (mode ? " 1234" : " Setter 1234"));
            index = complete(id, index + 1);
            require(tag(events[index], "command") == optional,
                    "Unambiguous supplemental metadata did not retain the explicit command origin");
        }
        request("MODE #modes", "mode");
        peer.send(":server 324 " + recipient + " #modes +nt");
        index = response("324", index + 1);
        require(tag(events[index], "command") == "324" && tag(events[index], "kind") == "mode" &&
                    tag(events[index], "command-id") == "mode" &&
                    tag(events[index], "command-response") == "324 #modes +nt",
                "MODE primary reply must release the active query and retain state semantics");
        request("MODE #modes", "mode-fresh");
        complete("mode", index + 1);
        peer.send(":server 329 " + recipient + " #modes 1234");
        index = response("329", index + 1);
        require(tag(events[index], "command-id").empty() &&
                    tag(events[index], "command-complete").empty() &&
                    events[index].params == vector<string>({recipient, "#modes", "1234"}),
                "Late optional creation numeric stole a fresh MODE query or lost state params");
        peer.send(":server 324 " + recipient + " #modes +n");
        index = response("324", index + 1);
        require(tag(events[index], "command-id") == "mode-fresh" &&
                    tag(events[index], "command") == "324" && tag(events[index], "kind") == "mode",
                "Missing optional creation numeric poisoned a later MODE query");
        peer.send(":server 329 " + recipient + " #modes 5678");
        index = response("329", index + 1);
        require(tag(events[index], "command-id").empty(),
                "An ambiguous old supplemental reply stole the fresh completed MODE context");
        request("MODE #modes +b mask", "mode-setter");
        peer.send(":server 367 " + recipient + " #modes unrelated-mask Setter 1234");
        index = response("367", index + 1);
        require(tag(events[index], "command-id").empty(),
                "MODE setter stole an unrelated ban list");
        peer.send(":server 368 " + recipient + " #modes :done");
        index = response("368", index + 1);
        require(tag(events[index], "command-id").empty(), "MODE setter stole ban list completion");
        request("MODE #modes +b", "ban-query");
        peer.send(":server 367 " + recipient + " #modes requested-mask Setter 1234");
        index = response("367", index + 1);
        require(tag(events[index], "command-id") == "ban-query",
                "Explicit ban query was not routed");
        peer.send(":server 368 " + recipient + " #modes :done");
        complete("ban-query", index + 1);
        request("TOPIC #topic", "topic");
        peer.send(":server 332 " + recipient + " #topic :Existing topic");
        index = response("332", index + 1);
        require(tag(events[index], "command-id") == "topic" &&
                    tag(events[index], "command") == "332" &&
                    events[index].type == "topic_current" && events[index].target == "#topic" &&
                    events[index].text == "Existing topic",
                "TOPIC primary reply must release the active query and retain state semantics");
        request("TOPIC #topic", "topic-fresh");
        complete("topic", index + 1);
        peer.send(":server 333 " + recipient + " #topic Setter 1234");
        index = response("333", index + 1);
        require(tag(events[index], "command-id").empty() &&
                    tag(events[index], "command-complete").empty() &&
                    events[index].params == vector<string>({recipient, "#topic", "Setter", "1234"}),
                "Late optional setter numeric stole a fresh TOPIC query or lost state params");
        peer.send(":server 332 " + recipient + " #topic :Fresh topic");
        index = response("332", index + 1);
        require(tag(events[index], "command-id") == "topic-fresh" &&
                    tag(events[index], "command") == "332" &&
                    events[index].type == "topic_current" && events[index].target == "#topic" &&
                    events[index].text == "Fresh topic",
                "Missing optional setter numeric poisoned a later TOPIC query");
        peer.send(":server 333 " + recipient + " #topic OldSetter 5678");
        index = response("333", index + 1);
        require(tag(events[index], "command-id").empty(),
                "An ambiguous old supplemental reply stole the fresh completed TOPIC context");
        request("TOPIC #empty-topic", "empty-topic");
        peer.send(":server 331 " + recipient + " #empty-topic :No topic is set");
        index = response("331", index + 1);
        require(tag(events[index], "command-id") == "empty-topic" &&
                    events[index].type == "topic_current" &&
                    events[index].target == "#empty-topic" && events[index].text.empty(),
                "No-topic reply must clear current topic and preserve command correlation");
        request("MODE #optional-background", "");
        request("MODE #optional-background", "optional-background");
        peer.send(":server 324 " + recipient + " #optional-background +nt");
        index = response("324", index + 1);
        require(tag(events[index], "command-id").empty(),
                "An explicit MODE query stole a background query's primary response");
        peer.send(":server 324 " + recipient + " #optional-background +n");
        index = response("324", index + 1);
        require(tag(events[index], "command-id").empty(),
                "Overlapping MODE primary responses were assigned by arrival order");
        complete("optional-background");
        peer.send(":server 329 " + recipient + " #optional-background 1234");
        index = response("329", index + 1);
        require(tag(events[index], "command-id").empty(),
                "A background MODE cohort's optional metadata acquired an explicit origin");
        request("TOPIC #optional-expiry", "optional-expiry");
        peer.send(":server 332 " + recipient + " #optional-expiry :No setter numeric");
        index = response("332", index + 1);
        require(tag(events[index], "command-id") == "optional-expiry" &&
                    tag(events[index], "command-complete").empty(),
                "Missing optional metadata retired its origin before the short grace period");
        peer.quietUntil(Clock::now() + 5100ms);
        index = complete("optional-expiry", index + 1);
        require(tag(events[index], "kind") == "command-complete" &&
                    tag(events[index], "command-response").empty() &&
                    tag(events[index], "error").empty(),
                "Optional metadata expiry must silently retire the origin without a timeout error");
        request("TOPIC #optional-expiry", "after-optional-expiry");
        peer.send(":server 332 " + recipient + " #optional-expiry :After expiry");
        index = response("332", index + 1);
        require(tag(events[index], "command-id") == "after-optional-expiry",
                "Expired optional metadata context poisoned a fresh primary response");
        peer.send(":server 333 " + recipient + " #optional-expiry Setter 1234");
        index = response("333", index + 1);
        require(tag(events[index], "command-id").empty(),
                "An expired origin's late optional reply stole the newer completed context");

        request("WHOIS Missing", "error");
        peer.send(":server 401 " + recipient + " Missing :No such nick");
        index = complete("error");
        require(tag(events[index], "command-response") == "401 Missing No such nick",
                "Fallback error did not finish the matching query");
        request("WHOIS Other", "unrelated-cohort");
        peer.send(":server 318 " + recipient + " Other :Unrelated query completes");
        index = complete("unrelated-cohort", index + 1);
        require(tag(events[index], "command-response") == "318 Other Unrelated query completes",
                "A literal error tombstone blocked an unrelated target");
        request("WHOIS Missing", "after-error");
        peer.send(":server 318 " + recipient + " Missing :Old error's trailing terminator");
        index = response("318", index + 1);
        require(tag(events[index], "command-id").empty(),
                "An old error's trailing reply completed a fresh command");
        peer.send(":server 311 " + recipient + " Missing u h * :Still indistinguishable");
        index = response("311", index + 1);
        require(tag(events[index], "command-id").empty(),
                "An overlapping error cohort reassigned late output");
        peer.send(":server 318 " + recipient + " Missing :Fresh command complete");
        complete("after-error", index + 1);
        request("WHOIS Duplicate", "overlap-one");
        request("WHOIS duplicate", "overlap-two");
        peer.send(":server 311 " + recipient + " Duplicate u h * :Ambiguous");
        index = response("311", index + 1);
        require(tag(events[index], "command-id").empty(), "Overlapping requests were guessed");
        require(tag(events[index], "command-name") == "WHOIS",
                "Ambiguous WHOIS lost no-output family without gaining a request");
        peer.send(":server 318 " + recipient + " Duplicate :done");
        index = response("318", index + 1);
        require(tag(events[index], "command-id").empty(), "Ambiguous terminal acquired a route");
        peer.send(":server 318 " + recipient + " Duplicate :done");
        complete("overlap-two", index + 1);
        request("WHOIS Duplicate", "after-overlap");
        peer.send(":server 318 " + recipient + " Duplicate :fresh request");
        complete("after-overlap", index + 1);

        peer.send(":" + recipient + "!u@h JOIN #background");
        peer.expect("WHO", {"#background"});
        request("WHO #background", "background-who");
        request("NAMES #background", "background-names");
        peer.send(":server 352 " + recipient + " #background u h server Person H :0 Real Name");
        index = response("352", index + 1);
        require(tag(events[index], "command-id").empty(), "Automatic WHO was stolen");
        peer.send(":server 353 " + recipient + " = #background :Person");
        index = response("353", index + 1);
        require(tag(events[index], "command-id").empty(), "Implicit NAMES was stolen");
        peer.send(":server 315 " + recipient + " #background :done");
        peer.send(":server 366 " + recipient + " #background :done");
        peer.send(":server 315 " + recipient + " #background :done");
        peer.send(":server 366 " + recipient + " #background :done");
        complete("background-who", index + 1);
        complete("background-names", index + 1);
        request("NAMES #background", "after-background");
        peer.send(":server 353 " + recipient + " = #background :Fresh");
        index = response("353", index + 1);
        require(tag(events[index], "command-id") == "after-background",
                "Finished background queries permanently blocked subsequent requests");
        peer.send(":server 366 " + recipient + " #background :done");
        complete("after-background", index + 1);

        request("WHOIS Waiting", "waiting");
        peer.send("@command-id=forged;command-complete=1;command-response=forged "
                  ":server NOTICE " +
                  recipient + " :unsolicited");
        index = waitEvent(client, events, index + 1, [](const Event &e) {
            return e.type == "notice" && e.text == "unsolicited";
        });
        require(tag(events[index], "command-id").empty() &&
                    tag(events[index], "command-response").empty(),
                "Unsolicited notice or forged local tags acquired a route");
        peer.send(":server 311 " + recipient + " Stranger u h * :Unsolicited");
        index = response("311", index + 1);
        require(tag(events[index], "command-id").empty(), "Unrelated numeric acquired a route");
        peer.send(":server 318 " + recipient + " Waiting :done");
        complete("waiting", index + 1);
    }
    client.command("PRIVMSG Recipient :submission body", "send-submission", "raw-send");
    peer.expect("PRIVMSG", {"Recipient", "submission body"});
    const auto handoff = complete("raw-send");
    require(tag(events[handoff], "kind") == "command-complete",
            "Raw text send did not hand off its command route");
    waitEvent(client, events, 0, [](const Event &e) {
        return tag(e, "submission") == "send-submission" && tag(e, "submission-complete") == "1";
    });
    client.command("PASS never-echo-this\r\nWHOIS Injected", "", "local-reject");
    auto index = complete("local-reject");
    require(events[index].type == "error" &&
                events[index].text.find("never-echo-this") == string::npos &&
                !events[index].tags.contains("command-response"),
            "Local rejection lost correlation or echoed the outbound command");
    if (labeled) {
        for (int i = 0; i <= 128; ++i)
            client.command("WHOIS Limit" + std::to_string(i), "", "limit-" + std::to_string(i));
        string retiredLabel;
        for (int i = 0; i <= 128; ++i) {
            const auto message = peer.expect("WHOIS", {"Limit" + std::to_string(i)});
            require(message.tags.contains("label"), "Bounded request lost its wire label");
            if (i == 0)
                retiredLabel = message.tags.at("label");
        }
        index = complete("limit-0", index + 1);
        require(tag(events[index], "kind") == "command-complete" &&
                    !tag(events[index], "error").empty(),
                "Correlation bounds did not retire the oldest route");
        peer.send("@label=" + retiredLabel + " :server 318 " + recipient + " Limit0 :Late reply");
        index = response("318", index + 1);
        require(tag(events[index], "command-id").empty(), "Retired wire label was reassigned");
    }
    request("WHOIS Disconnected", "disconnect");
    peer.fence();
    client.stop();
    index = complete("disconnect", index + 1);
    require(tag(events[index], "kind") == "command-complete" &&
                !tag(events[index], "error").empty() && events[index].text.empty(),
            "Disconnect did not silently retire pending command routes");
}
} // namespace

int main(int argc, char **argv) {
    std::signal(SIGPIPE, SIG_IGN);
    const bool avatar = argc == 4 && string(argv[1]) == "--avatar";
    const bool soju = argc == 2 && string(argv[1]) == "--soju";
    if (argc != 1 && !avatar && !soju) {
        std::cerr << "usage: network-check [--avatar PUBLIC_URL OUTPUT | --soju]\n";
        return 1;
    }
    // The full suite includes two 35s history deadline checks plus metadata/expiry
    // waits and TLS/frame I/O. Keep per-operation deadlines; bound total shutdown too.
    ::alarm(avatar ? 40 : soju ? 20 : 180);
    const char *phase = "TLS fixture setup";
    try {
        TemporaryDirectory directory;
        const auto certificate = directory.path / "localhost.pem";
        auto context = trustedLocalhost(certificate);
        Environment config("XDG_CONFIG_HOME", directory.path / "config");
        Environment data("XDG_DATA_HOME", directory.path / "data");
        Identity identity;
        identity.nick = identity.username = "wireuser";
        identity.password = "network-check-only-secret";
        if (avatar) {
            phase = "runtime channel avatar";
            avatarSmoke(context.get(), identity, certificate, argv[2], argv[3]);
            return 0;
        }
        Environment trust("SSL_CERT_FILE", certificate);
        // Nix OpenSSL prioritizes this over SSL_CERT_FILE; isolate both trust inputs.
        Environment nixTrust("NIX_SSL_CERT_FILE", certificate);
        if (soju) {
            phase = "soju catalog / CRUD / redaction";
            sojuChecks(context.get(), identity);
            std::cout
                << "production soju catalog, CRUD, authentication and redaction checks passed\n";
            return 0;
        }
        phase = "invalid nickname / Unicode registration";
        registrationValidationChecks(context.get());
        phase = "SASL / STS";
        stsChecks(context.get(), identity);
        phase = "CLIENTTAGDENY";
        clientTagPolicyChecks(context.get(), identity);
        phase = "reply wire fallback";
        replyFallbackChecks(context.get(), identity);
        phase = "queued reply revocation / no-echo retirement";
        queuedReplyRevocationChecks(context.get(), identity);
        phase = "RENAME";
        renameChecks(context.get(), identity);
        phase = "full-write status / bounded transport";
        sentChecks(context.get(), identity);
        blockedSentChecks(context.get(), identity, true, false);
        blockedSentChecks(context.get(), identity, true, true);
        blockedSentChecks(context.get(), identity, false, true);
        blockedSentChecks(context.get(), identity, true, false, true);
        phase = "channel metadata / postponed synchronization";
        preReadyMetadataChecks(context.get(), identity);
        phase = "history consumer wire / discovery / root validation / selector retry";
        historyConsumerWireChecks(context.get(), identity, false);
        historyConsumerWireChecks(context.get(), identity, true);
        phase = "history ranking / correlated admission / disconnect cleanup";
        historyQueueChecks(context.get(), identity, false);
        historyQueueChecks(context.get(), identity, true);
        metadataChecks(context.get(), identity);
        metadataLookupChecks(context.get(), identity);
        phase = "ZNC playback";
        playbackChecks(context.get(), identity);
        phase = "command reply correlation";
        commandCorrelationChecks(context.get(), identity, true);
        commandCorrelationChecks(context.get(), identity, false);
        std::cout << "production TLS/SASL/STS, RENAME, full-write status, channel metadata and ZNC "
                     "playback checks passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "network-check [" << phase << "]: " << error.what() << '\n';
        return 1;
    }
}
