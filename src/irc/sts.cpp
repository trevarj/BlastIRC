#include "sts.hpp"

#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace blast::irc {
namespace {

constexpr std::size_t maxEntries = 512;
constexpr std::size_t maxBytes = 128 * 1024;
// ponytail: serialize this tiny bounded store; per-directory mutexes only if needed.
std::mutex storeMutex;

struct Fd {
    int value;
    explicit Fd(int fd) : value(fd) {}
    ~Fd() {
        if (value >= 0)
            ::close(value);
    }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
};

[[noreturn]] void ioError(const char *operation) {
    const int error = errno;
    throw std::runtime_error(std::string("STS ") + operation + ": " + std::strerror(error));
}

bool validHost(std::string_view host) {
    if (host.empty() || host.size() > 253)
        return false;
    for (unsigned char c : host) {
        if (c <= 0x20 || c == 0x7f)
            return false;
    }
    return true;
}

std::string hostKey(std::string_view host) {
    std::string key(host);
    for (char &c : key) {
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
    }
    return key;
}

int64_t nowSeconds() {
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    if (now < 0)
        throw std::runtime_error("STS system clock predates the Unix epoch");
    return now;
}

std::filesystem::path configDirectory() {
    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0] == '/')
        return std::filesystem::path(xdg) / "blastirc";
    const char *home = std::getenv("HOME");
    if (!home || home[0] != '/') {
        throw std::runtime_error("STS requires an absolute HOME or XDG_CONFIG_HOME");
    }
    return std::filesystem::path(home) / ".config" / "blastirc";
}

void createDirectory(const std::filesystem::path &directory) {
    std::filesystem::path current;
    for (const auto &component : directory) {
        current /= component;
        if (::mkdir(current.c_str(), 0700) == 0) {
            // Set the exact private mode even under a restrictive umask.
            if (::chmod(current.c_str(), 0700) != 0)
                ioError("setting directory permissions");
        } else if (errno != EEXIST) {
            ioError("creating configuration directory");
        }
    }
}

void lockDirectory(int fd, int operation) {
    // Lock the stable directory inode, not sts: atomic replacement changes its inode.
    while (::flock(fd, operation) != 0) {
        if (errno != EINTR)
            ioError("locking configuration directory");
    }
}

using Policies = std::map<std::string, StsPolicy>;

Policies readPolicies(int directory) {
    Fd file(::openat(directory, "sts", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (file.value < 0) {
        if (errno == ENOENT)
            return {};
        ioError("opening policy file");
    }
    struct stat info{};
    if (::fstat(file.value, &info) != 0)
        ioError("examining policy file");
    if (!S_ISREG(info.st_mode) || info.st_size < 0 ||
        static_cast<uintmax_t>(info.st_size) > maxBytes) {
        throw std::runtime_error("STS policy file is not regular or exceeds 128 KiB");
    }
    std::string contents;
    contents.reserve(static_cast<std::size_t>(info.st_size));
    std::array<char, 4096> buffer;
    for (;;) {
        const ssize_t count = ::read(file.value, buffer.data(), buffer.size());
        if (count < 0) {
            if (errno == EINTR)
                continue;
            ioError("reading policy file");
        }
        if (count == 0)
            break;
        if (static_cast<std::size_t>(count) > maxBytes - contents.size()) {
            throw std::runtime_error("STS policy file exceeds 128 KiB");
        }
        contents.append(buffer.data(), static_cast<std::size_t>(count));
    }

    Policies policies;
    std::string_view remaining(contents);
    while (!remaining.empty()) {
        const auto newline = remaining.find('\n');
        const auto line = remaining.substr(0, newline);
        remaining =
            newline == std::string_view::npos ? std::string_view{} : remaining.substr(newline + 1);
        const auto first = line.find('\t');
        if (first == std::string_view::npos || !validHost(line.substr(0, first)))
            continue;
        const auto second = line.find('\t', first + 1);
        if (second == std::string_view::npos)
            continue;
        const auto portText = line.substr(first + 1, second - first - 1);
        const auto expiryText = line.substr(second + 1);
        StsPolicy policy{};
        const auto port =
            std::from_chars(portText.data(), portText.data() + portText.size(), policy.port);
        const auto expiry = std::from_chars(expiryText.data(),
                                            expiryText.data() + expiryText.size(), policy.expires);
        if (port.ec != std::errc{} || port.ptr != portText.data() + portText.size() ||
            expiry.ec != std::errc{} || expiry.ptr != expiryText.data() + expiryText.size() ||
            policy.port < 1 || policy.port > 65535 || policy.expires <= 0)
            continue;
        auto [entry, inserted] = policies.try_emplace(hostKey(line.substr(0, first)), policy);
        // Duplicate or malformed lines must not shorten an existing valid policy.
        if (!inserted && policy.expires > entry->second.expires)
            entry->second = policy;
        if (policies.size() > maxEntries)
            throw std::runtime_error("STS policy file exceeds 512 hosts");
    }
    return policies;
}

void syncFd(int fd, const char *operation) {
    while (::fsync(fd) != 0) {
        if (errno != EINTR)
            ioError(operation);
    }
}

void persist(int directory, const Policies &policies) {
    std::string contents;
    for (const auto &[host, policy] : policies) {
        contents += host;
        contents += '\t';
        contents += std::to_string(policy.port);
        contents += '\t';
        contents += std::to_string(policy.expires);
        contents += '\n';
        if (contents.size() > maxBytes)
            throw std::runtime_error("STS policies exceed 128 KiB");
    }

    // O_EXCL and the private directory keep temporary files private and race-free.
    static uint64_t serial = 0;
    std::string temporary;
    Fd file(-1);
    for (int attempt = 0; attempt < 512; ++attempt) {
        temporary = ".sts." + std::to_string(::getpid()) + "." + std::to_string(++serial);
        file.value = ::openat(directory, temporary.c_str(),
                              O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (file.value >= 0)
            break;
        if (errno != EEXIST)
            ioError("creating temporary policy file");
    }
    if (file.value < 0)
        throw std::runtime_error("STS cannot allocate a temporary policy file");
    try {
        if (::fchmod(file.value, 0600) != 0)
            ioError("setting policy file permissions");
        std::string_view remaining(contents);
        while (!remaining.empty()) {
            const ssize_t written = ::write(file.value, remaining.data(), remaining.size());
            if (written < 0) {
                if (errno == EINTR)
                    continue;
                ioError("writing policy file");
            }
            if (written == 0)
                throw std::runtime_error("STS policy write made no progress");
            remaining.remove_prefix(static_cast<std::size_t>(written));
        }
        syncFd(file.value, "syncing policy file");
        if (::close(std::exchange(file.value, -1)) != 0)
            ioError("closing policy file");
        if (::renameat(directory, temporary.c_str(), directory, "sts") != 0)
            ioError("replacing policy file");
        syncFd(directory, "syncing configuration directory");
    } catch (...) {
        ::unlinkat(directory, temporary.c_str(), 0);
        throw;
    }
}

} // namespace

std::optional<StsPolicy> loadSts(std::string_view host) {
    if (!validHost(host))
        throw std::invalid_argument("Invalid STS host");
    const auto key = hostKey(host);
    const std::lock_guard lock(storeMutex);
    const auto path = configDirectory();
    Fd directory(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (directory.value < 0) {
        if (errno == ENOENT)
            return std::nullopt;
        ioError("opening configuration directory");
    }
    lockDirectory(directory.value, LOCK_SH);
    const auto policies = readPolicies(directory.value);
    const auto entry = policies.find(key);
    if (entry == policies.end() || entry->second.expires <= nowSeconds())
        return std::nullopt;
    return entry->second;
}

void saveSts(std::string_view host, int port, int64_t durationSeconds) {
    if (!validHost(host) || port < 1 || port > 65535 || durationSeconds < 0) {
        throw std::invalid_argument("Invalid STS host, port or duration");
    }
    const auto key = hostKey(host);
    const std::lock_guard lock(storeMutex);
    const auto path = configDirectory();
    createDirectory(path);
    Fd directory(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (directory.value < 0)
        ioError("opening configuration directory");
    lockDirectory(directory.value, LOCK_EX);
    const auto now = nowSeconds();
    if (durationSeconds > std::numeric_limits<int64_t>::max() - now) {
        throw std::invalid_argument("STS duration overflows its expiry");
    }
    auto policies = readPolicies(directory.value);
    for (auto entry = policies.begin(); entry != policies.end();) {
        if (entry->second.expires <= now)
            entry = policies.erase(entry);
        else
            ++entry;
    }
    if (durationSeconds == 0)
        policies.erase(key);
    else
        policies.insert_or_assign(key, StsPolicy{port, now + durationSeconds});
    if (policies.size() > maxEntries)
        throw std::runtime_error("STS policy store is full (512 hosts)");
    persist(directory.value, policies);
}

} // namespace blast::irc
