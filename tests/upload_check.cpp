#include "upload.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>
using namespace blast;
namespace {
struct Temp {
    std::string path;
    explicit Temp(std::string_view data = {}) {
        char name[] = "/tmp/blast-upload-XXXXXX";
        int fd = mkstemp(name);
        assert(fd >= 0);
        path = name;
        assert(::write(fd, data.data(), data.size()) == static_cast<ssize_t>(data.size()));
        close(fd);
    }
    ~Temp() { unlink(path.c_str()); }
};
struct Certificate {
    EVP_PKEY *key = nullptr;
    X509 *cert = nullptr;
    Temp ca;
    Certificate() {
        auto *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
        assert(ctx);
        assert(EVP_PKEY_keygen_init(ctx) == 1 && EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) == 1);
        assert(EVP_PKEY_keygen(ctx, &key) == 1);
        EVP_PKEY_CTX_free(ctx);
        cert = X509_new();
        assert(cert);
        X509_set_version(cert, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
        X509_gmtime_adj(X509_get_notBefore(cert), -60);
        X509_gmtime_adj(X509_get_notAfter(cert), 3600);
        X509_set_pubkey(cert, key);
        auto *name = X509_get_subject_name(cert);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char *>("localhost"), -1, -1, 0);
        X509_set_issuer_name(cert, name);
        auto extension = [&](int nid, const char *text) {
            auto *e = X509V3_EXT_conf_nid(nullptr, nullptr, nid, const_cast<char *>(text));
            assert(e);
            assert(X509_add_ext(cert, e, -1));
            X509_EXTENSION_free(e);
        };
        extension(NID_basic_constraints, "critical,CA:TRUE");
        extension(NID_subject_alt_name, "DNS:localhost,IP:127.0.0.1");
        assert(X509_sign(cert, key, EVP_sha256()));
        FILE *out = fopen(ca.path.c_str(), "w");
        assert(out);
        assert(PEM_write_X509(out, cert));
        fclose(out);
    }
    ~Certificate() {
        X509_free(cert);
        EVP_PKEY_free(key);
    }
};
struct Server {
    SSL_CTX *context;
    int listener;
    std::thread thread;
    std::string endpoint, wire;
    std::atomic<bool> gotBody{false};
    Server(Certificate &cert, std::string response, bool stall = false,
           std::function<void()> onBody = {}) {
        context = SSL_CTX_new(TLS_server_method());
        assert(context);
        assert(SSL_CTX_use_certificate(context, cert.cert) == 1 &&
               SSL_CTX_use_PrivateKey(context, cert.key) == 1);
        listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        assert(listener >= 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        assert(bind(listener, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0 &&
               listen(listener, 2) == 0);
        socklen_t length = sizeof(addr);
        assert(getsockname(listener, reinterpret_cast<sockaddr *>(&addr), &length) == 0);
        endpoint = "https://localhost:" + std::to_string(ntohs(addr.sin_port)) + "/api";
        thread = std::thread([this, response = std::move(response), stall, onBody] {
            int client = accept(listener, nullptr, nullptr);
            if (client < 0)
                return;
            timeval timeout{5, 0};
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
            SSL *ssl = SSL_new(context);
            SSL_set_fd(ssl, client);
            if (SSL_accept(ssl) == 1) {
                std::size_t wanted = 0, headerEnd = std::string::npos;
                char buffer[8192];
                while (true) {
                    int n = SSL_read(ssl, buffer, sizeof(buffer));
                    if (n <= 0)
                        break;
                    wire.append(buffer, n);
                    if (headerEnd == std::string::npos) {
                        headerEnd = wire.find("\r\n\r\n");
                        if (headerEnd != std::string::npos) {
                            auto p = wire.find("Content-Length:");
                            assert(p != std::string::npos);
                            wanted = std::stoull(wire.substr(p + 15));
                            headerEnd += 4;
                        }
                    }
                    if (headerEnd != std::string::npos && wire.size() >= headerEnd + wanted) {
                        gotBody = true;
                        if (onBody)
                            onBody();
                        if (stall) {
                            // Keep the response outstanding until the client cancels/closes.
                            SSL_read(ssl, buffer, sizeof(buffer));
                        } else {
                            std::size_t offset = 0;
                            while (offset < response.size()) {
                                int count = SSL_write(ssl, response.data() + offset,
                                                      response.size() - offset);
                                if (count <= 0)
                                    break;
                                offset += count;
                            }
                        }
                        break;
                    }
                }
            }
            SSL_free(ssl);
            close(client);
        });
    }
    ~Server() {
        shutdown(listener, SHUT_RDWR);
        close(listener);
        if (thread.joinable())
            thread.join();
        SSL_CTX_free(context);
    }
    void join() {
        if (thread.joinable())
            thread.join();
    }
};
std::string response(std::string body, std::string headers = {}, int status = 200) {
    return "HTTP/1.1 " + std::to_string(status) +
           " Result\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\n" + headers + "\r\n" + body;
}
UploadResult finish(Uploader &uploader) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto result = uploader.takeResult())
            return *result;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    assert(false && "upload failed to settle");
    return {};
}
UploadRequest request(Temp &file, Certificate &cert, Server &server, UploadProvider provider) {
    UploadRequest r;
    r.path = file.path;
    r.mime = "text/plain";
    r.tlsCaFile = cert.ca.path;
    r.endpoint = server.endpoint;
    r.config.provider = provider;
    r.reviewedFile = inspectUploadFile(r.path);
    if (provider == UploadProvider::Custom)
        r.config.customEndpoint = server.endpoint;
    if (provider == UploadProvider::Soju) {
        r.credentialHost = "localhost";
        r.config.username = "account";
        r.config.password = "private-password";
    }
    return r;
}
void contains(const std::string &text, std::string_view value) {
    assert(text.find(value) != std::string::npos);
}
void absent(const std::string &text, std::string_view value) {
    assert(text.find(value) == std::string::npos);
}
} // namespace
int main() {
    signal(SIGPIPE, SIG_IGN);
    assert(uploadEndpoint(UploadConfig{}) == "https://crafterbin.glennstack.dev");
    assert(UploadConfig{}.litterboxExpiry == "24h");
    {
        UploadConfig c;
        c.provider = UploadProvider::Custom;
        c.customEndpoint = "https://example.org/#upload";
        assert(!validateUploadConfig(c).empty());
    }
    for (int i = 0; i != 8; ++i)
        assert(parseUploadProvider(uploadProviderToken(static_cast<UploadProvider>(i))) ==
               static_cast<UploadProvider>(i));
    assert(!safeUploadUrl("https://user:secret@example.org/a"));
    assert(!safeUploadUrl("https://example.org/a\r\nPRIVMSG"));
    assert(!safeUploadUrl("https://example.org/%0a"));
    assert(!safeUploadUrl("http://example.org/a"));
    assert(safeUploadUrl("http://example.org/a", false));
    assert(trustedSojuUploadEndpoint("https://files.example.org/x", "example.org"));
    assert(!trustedSojuUploadEndpoint("https://notexample.org/x", "example.org"));
    assert(!trustedSojuUploadEndpoint("https://example.org.evil/x", "example.org"));
    assert(trustedSojuUploadEndpoint("https://private.test/x", "example.org", "private.test"));
    assert(!trustedSojuUploadEndpoint("https://evil.127.0.0.1/x", "127.0.0.1"));
    assert(!sameUploadAuthority("https://example.org", "https://example.org:444"));
    Certificate cert;
    Temp file("streamed-file-data\n");
    const std::string link = "https://files.example.org/result";
    // Every provider gets a real TLS exchange; inspect protocol fields and actual streamed bytes.
    for (int i = 0; i != 8; ++i) {
        auto p = static_cast<UploadProvider>(i);
        auto body = p == UploadProvider::Uguu ? "{\"files\":[{\"url\":\"" + link + "\"}]}"
                    : p == UploadProvider::CNet
                        ? "{\"url\":\"" + link + "\",\"delete_key\":\"not-retained\"}"
                        : link + "\n";
        Server server(cert,
                      response(body, p == UploadProvider::Soju ? "Location: " + link + "\r\n" : "",
                               p == UploadProvider::Soju ? 201 : 200));
        auto r = request(file, cert, server, p);
        if (p == UploadProvider::Custom) {
            r.config.username = "account";
            r.config.password = "private-password";
        }
        Uploader uploader;
        assert(uploader.start(r));
        assert(!uploader.start(r));
        auto result = finish(uploader);
        assert(result && result.url == link);
        server.join();
        auto progress = uploader.takeProgress();
        assert(progress && progress->sent == 19 && progress->total == 19 && progress->complete);
        assert(!uploader.takeProgress());
        contains(server.wire, "streamed-file-data\n");
        absent(result.message, "private-password");
        if (p == UploadProvider::CNet || p == UploadProvider::Soju) {
            assert(server.wire.starts_with(p == UploadProvider::CNet ? "PUT " : "POST "));
            contains(server.wire, "Content-Type: text/plain");
            if (p == UploadProvider::CNet) {
                contains(server.wire, "X-FileName:");
                contains(server.wire, "X-UUID:");
                contains(server.wire, "Accept: application/json");
            } else {
                contains(server.wire, "Content-Disposition: attachment; filename=\"");
                contains(server.wire, "Authorization: Basic ");
            }
        } else {
            contains(server.wire, "multipart/form-data");
            contains(server.wire, p == UploadProvider::Uguu ? "name=\"files[]\""
                                  : (p == UploadProvider::Litterbox || p == UploadProvider::Catbox)
                                      ? "name=\"fileToUpload\""
                                      : "name=\"file\"");
            if (p == UploadProvider::CrafterBin || p == UploadProvider::Custom) {
                contains(server.wire, "name=\"secret\"");
                contains(server.wire, "name=\"expires\"\r\n\r\n168");
            } else {
                absent(server.wire, "name=\"secret\"");
                absent(server.wire, "name=\"expires\"");
            }
            if (p == UploadProvider::Litterbox || p == UploadProvider::Catbox)
                contains(server.wire, "name=\"reqtype\"\r\n\r\nfileupload");
            if (p == UploadProvider::Litterbox)
                contains(server.wire, "name=\"time\"\r\n\r\n24h");
            if (p == UploadProvider::Custom) {
                contains(server.wire, "name=\"expiration\"\r\n\r\nnever");
                contains(server.wire, "name=\"burn_after\"\r\n\r\n0");
                contains(server.wire, "name=\"privacy\"\r\n\r\nunlisted");
                contains(server.wire, "name=\"syntax_highlight\"\r\n\r\nnone");
                contains(server.wire, "name=\"uploader_password\"\r\n\r\nprivate-password");
                contains(server.wire, "Authorization: Basic ");
            } else
                absent(server.wire, "Authorization:");
        }
    }
    {
        Server server(cert, response(link));
        Uploader uploader;
        auto r = request(file, cert, server, UploadProvider::Custom);
        r.config.secret = false;
        r.config.expiryHours.clear();
        r.config.password = "password-only";
        assert(uploader.start(r));
        assert(finish(uploader));
        server.join();
        absent(server.wire, "name=\"secret\"");
        absent(server.wire, "name=\"expires\"");
        absent(server.wire, "Authorization:");
        contains(server.wire, "name=\"uploader_password\"\r\n\r\npassword-only");
    }
    auto exchange = [&](UploadProvider p, std::string body, std::string headers, int status,
                        std::function<void(UploadRequest &)> adjust = {},
                        std::function<void()> onBody = {}) {
        Server server(cert, response(std::move(body), std::move(headers), status), false, onBody);
        auto r = request(file, cert, server, p);
        if (adjust)
            adjust(r);
        Uploader uploader;
        assert(uploader.start(r));
        return finish(uploader);
    };
    auto result = exchange(UploadProvider::Custom, "", "Location: /upload/slug\r\n", 303);
    assert(result && result.url.ends_with("/file/slug"));
    result =
        exchange(UploadProvider::Custom, "", "Location: https://evil.invalid/upload/slug\r\n", 302);
    assert(result.error == UploadError::Response);
    absent(result.message, "evil.invalid");
    result = exchange(UploadProvider::Custom, "", "Location: /incorrect\r\n", 302);
    assert(result.error == UploadError::Http);
    result = exchange(UploadProvider::Custom, "", "Location: /other\r\n", 307);
    assert(result.error == UploadError::Response);
    result = exchange(UploadProvider::Soju, "", "Location: /relative\r\n", 201);
    assert(result && result.url.ends_with("/relative"));
    result = exchange(UploadProvider::Soju, "", "Location: /relative\r\n", 200);
    assert(result.error == UploadError::Http);
    result = exchange(UploadProvider::Soju, "", "", 201);
    assert(result.error == UploadError::Response);
    result = exchange(UploadProvider::Soju, "", "Location: http://example.org/file\r\n", 201);
    assert(result.error == UploadError::Response);
    result = exchange(UploadProvider::Soju, "", "Location: /one\r\nLocation: /two\r\n", 201);
    assert(result.error == UploadError::Response);
    result = exchange(UploadProvider::CrafterBin, link, "Location: /retry\r\n", 302);
    assert(result.error == UploadError::Http);
    result = exchange(UploadProvider::CrafterBin, "secret-body-token", "", 500);
    assert(result.error == UploadError::Http);
    absent(result.message, "secret-body-token");
    for (auto body : {"not-json", "{\"files\":null}", "{\"files\":[7]}",
                      "{\"files\":[{\"url\":\"https://example.org/x\\nPRIVMSG\"}]}"}) {
        result = exchange(UploadProvider::Uguu, body, "", 200);
        assert(result.error == UploadError::Response);
    }
    result = exchange(UploadProvider::CNet, "{\"url\":42}", "", 200);
    assert(result.error == UploadError::Response);
    result =
        exchange(UploadProvider::Uguu, std::string(32, '[') + "0" + std::string(32, ']'), "", 200);
    assert(result.error == UploadError::Response);
    result = exchange(UploadProvider::CrafterBin, std::string(65536, 'x'), "", 200);
    assert(result.error == UploadError::Response);
    result = exchange(UploadProvider::CrafterBin, link,
                      "X-Large: " + std::string(65536, 'x') + "\r\n", 200);
    assert(result.error == UploadError::Response);
    Certificate unrelated;
    result = exchange(UploadProvider::CrafterBin, link, "", 200,
                      [&](auto &r) { r.tlsCaFile = unrelated.ca.path; });
    assert(result.error == UploadError::Tls);
    // Initial size/regular-file/credential authority failures happen without connecting.
    for (int mode = 0; mode != 4; ++mode) {
        UploadRequest r;
        r.path = file.path;
        r.endpoint = "https://localhost:1/api";
        r.reviewedFile = inspectUploadFile(r.path);
        if (mode == 0)
            r.config.maxBytes = 1;
        if (mode == 1)
            r.path = "/dev/null";
        if (mode == 2)
            r.path = file.path + "/missing";
        if (mode == 3) {
            r.config.provider = UploadProvider::Soju;
            r.config.username = "private-user";
            r.config.password = "private-pass";
            r.credentialHost = "unrelated.invalid";
        }
        Uploader uploader;
        assert(uploader.start(r));
        result = finish(uploader);
        assert(result.error == (mode == 0   ? UploadError::Size
                                : mode == 3 ? UploadError::InvalidRequest
                                            : UploadError::File));
        absent(result.message, "private-user");
        absent(result.message, "private-pass");
    }
    // A replaced path cannot silently upload a different file than the reviewed one.
    {
        UploadRequest r;
        r.path = file.path;
        r.endpoint = "https://localhost:1/api";
        r.reviewedFile = inspectUploadFile(r.path);
        Temp replacement("different-file");
        assert(rename(replacement.path.c_str(), file.path.c_str()) == 0);
        Uploader uploader;
        assert(uploader.start(r));
        result = finish(uploader);
        assert(result.error == UploadError::File);
    }
    // Change after the final streamed byte is still detected before accepting the URL.
    result = exchange(UploadProvider::CrafterBin, link, "", 200, {}, [&] {
        std::ofstream out(file.path, std::ios::app);
        out << "changed";
    });
    assert(result.error == UploadError::File);
    result = exchange(UploadProvider::CrafterBin, link, "", 200, {},
                      [&] { assert(truncate(file.path.c_str(), 0) == 0); });
    assert(result.error == UploadError::File);
    {
        Server server(cert, "", true);
        Uploader uploader;
        auto r = request(file, cert, server, UploadProvider::CrafterBin);
        assert(uploader.start(r));
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!server.gotBody && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        assert(server.gotBody);
        uploader.cancel();
        result = finish(uploader);
        assert(result.error == UploadError::Cancelled);
        server.join();
    }
    {
        // A late Cancel between validated completion and result consumption cannot erase
        // the remotely created resource. Completion is semantic, not a sleep-based race.
        Server server(cert, response(link));
        Uploader uploader;
        auto r = request(file, cert, server, UploadProvider::CrafterBin);
        assert(uploader.start(r));
        bool completed = false;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!completed && std::chrono::steady_clock::now() < deadline) {
            if (auto progress = uploader.takeProgress())
                completed = progress->complete;
            if (!completed)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        assert(completed);
        uploader.cancel();
        result = finish(uploader);
        assert(result && result.url == link);
        server.join();
    }
    std::cout << "upload-check: local TLS protocols and boundaries passed\n";
}
