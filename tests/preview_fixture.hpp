#pragma once

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <mutex>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace preview_fixture {
using namespace std::chrono_literals;

struct TempDirectory {
    std::string path;
    TempDirectory() {
        char name[] = "/tmp/blast-preview-check-XXXXXX";
        assert(mkdtemp(name));
        path = name;
    }
    ~TempDirectory() { std::filesystem::remove_all(path); }
};
struct Certificate {
    EVP_PKEY *key = nullptr;
    X509 *cert = nullptr;
    std::string caPath;
    Certificate(const std::string &directory, std::string name = "localhost") {
        auto *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
        assert(ctx);
        assert(EVP_PKEY_keygen_init(ctx) == 1);
        assert(EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) == 1);
        assert(EVP_PKEY_keygen(ctx, &key) == 1);
        EVP_PKEY_CTX_free(ctx);
        cert = X509_new();
        assert(cert);
        assert(X509_set_version(cert, 2) == 1);
        assert(ASN1_INTEGER_set(X509_get_serialNumber(cert), 1) == 1);
        assert(X509_gmtime_adj(X509_get_notBefore(cert), -60));
        assert(X509_gmtime_adj(X509_get_notAfter(cert), 3600));
        assert(X509_set_pubkey(cert, key) == 1);
        auto *subject = X509_get_subject_name(cert);
        assert(X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                                          reinterpret_cast<const unsigned char *>(name.c_str()), -1,
                                          -1, 0) == 1);
        assert(X509_set_issuer_name(cert, subject) == 1);
        auto extension = [&](int nid, const std::string &value) {
            auto *ext =
                X509V3_EXT_conf_nid(nullptr, nullptr, nid, const_cast<char *>(value.c_str()));
            assert(ext);
            assert(X509_add_ext(cert, ext, -1) == 1);
            X509_EXTENSION_free(ext);
        };
        extension(NID_basic_constraints, "critical,CA:TRUE");
        extension(NID_key_usage, "critical,keyCertSign,digitalSignature,keyEncipherment");
        extension(NID_subject_alt_name,
                  name == "localhost" ? "DNS:localhost,IP:127.0.0.1" : "DNS:" + name);
        assert(X509_sign(cert, key, EVP_sha256()) > 0);
        caPath = directory + "/" + name + ".pem";
        FILE *file = fopen(caPath.c_str(), "w");
        assert(file);
        assert(PEM_write_X509(file, cert) == 1);
        assert(fclose(file) == 0);
    }
    ~Certificate() {
        X509_free(cert);
        EVP_PKEY_free(key);
    }
};
struct Reply {
    std::string body;
    std::string headers = "Content-Type: text/html\r\n";
    int status = 200;
    bool holdBody = false;
    bool omitLength = false;
};

// A private, ephemeral TLS listener: no real network, credentials, hosts or config files.
struct Server {
    SSL_CTX *context;
    int listener;
    std::string endpoint;
    std::atomic<unsigned> connections{0}, requests{0};
    std::atomic<bool> stop{false}, releaseBody{false};
    std::mutex mutex;
    std::vector<std::string> wires;
    std::thread thread;
    explicit Server(Certificate &cert, std::function<Reply(std::string_view)> reply) {
        context = SSL_CTX_new(TLS_server_method());
        assert(context);
        assert(SSL_CTX_use_certificate(context, cert.cert) == 1);
        assert(SSL_CTX_use_PrivateKey(context, cert.key) == 1);
        listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        assert(listener >= 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        assert(bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
        assert(listen(listener, 32) == 0);
        socklen_t size = sizeof(address);
        assert(getsockname(listener, reinterpret_cast<sockaddr *>(&address), &size) == 0);
        endpoint = "https://localhost:" + std::to_string(ntohs(address.sin_port));
        thread = std::thread([this, reply = std::move(reply)] {
            while (!stop) {
                pollfd pending{listener, POLLIN, 0};
                if (::poll(&pending, 1, 50) <= 0)
                    continue;
                int client = accept(listener, nullptr, nullptr);
                if (client < 0)
                    continue;
                ++connections;
                timeval timeout{2, 0};
                assert(setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
                assert(setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0);
                SSL *ssl = SSL_new(context);
                assert(ssl);
                assert(SSL_set_fd(ssl, client) == 1);
                if (SSL_accept(ssl) == 1) {
                    std::string wire;
                    char buffer[4096];
                    while (wire.size() < 32768 && wire.find("\r\n\r\n") == wire.npos) {
                        int n = SSL_read(ssl, buffer, sizeof(buffer));
                        if (n <= 0)
                            break;
                        wire.append(buffer, n);
                    }
                    if (wire.find("\r\n\r\n") != wire.npos) {
                        const auto begin = wire.find(' '), end = wire.find(' ', begin + 1);
                        assert(begin != wire.npos && end != wire.npos);
                        Reply response =
                            reply(std::string_view(wire).substr(begin + 1, end - begin - 1));
                        {
                            std::lock_guard lock(mutex);
                            wires.push_back(wire);
                        }
                        ++requests;
                        std::string headers = "HTTP/1.1 " + std::to_string(response.status) +
                                              " Fixture\r\nConnection: close\r\n" +
                                              response.headers;
                        if (!response.omitLength)
                            headers +=
                                "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
                        headers += "\r\n";
                        auto send = [&](const std::string &bytes) {
                            std::size_t offset = 0;
                            while (offset < bytes.size()) {
                                int n =
                                    SSL_write(ssl, bytes.data() + offset,
                                              std::min<std::size_t>(16384, bytes.size() - offset));
                                if (n <= 0)
                                    return;
                                offset += n;
                            }
                        };
                        send(headers);
                        while (response.holdBody && !releaseBody && !stop)
                            std::this_thread::sleep_for(2ms);
                        if (!stop)
                            send(response.body);
                        // Send close_notify so close-delimited responses aren't truncated TLS
                        // streams.
                        SSL_shutdown(ssl);
                    }
                }
                SSL_free(ssl);
                close(client);
            }
        });
    }
    ~Server() {
        stop = true;
        releaseBody = true;
        thread.join();
        close(listener);
        SSL_CTX_free(context);
    }
};

template <class Predicate> void until(Predicate ready) {
    const auto deadline = std::chrono::steady_clock::now() + 8s;
    while (!ready()) {
        assert(std::chrono::steady_clock::now() < deadline && "preview fixture timed out");
        std::this_thread::sleep_for(2ms);
    }
}
} // namespace preview_fixture
