#include "storage.hpp"
#include "irc/protocol.hpp"
#include "ui/logic.hpp"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iterator>
#include <limits>
#include <optional>
#include <pwd.h>
#include <sqlite3.h>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <toml++/toml.hpp>
#include <tuple>
#include <type_traits>
#include <unistd.h>

namespace blast {
namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::system_clock;
int64_t nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(Clock::now().time_since_epoch())
        .count();
}
[[noreturn]] void fail(const string &text) {
    throw std::runtime_error("BlastIRC storage: " + text);
}
[[noreturn]] void systemFail(const string &operation, const fs::path &path) {
    const int error = errno;
    fail(operation + " " + path.string() + ": " + std::strerror(error));
}
fs::path homeDirectory() {
    if (const char *home = std::getenv("HOME"); home && fs::path(home).is_absolute())
        return home;
    const passwd *account = getpwuid(geteuid());
    if (account && account->pw_dir && fs::path(account->pw_dir).is_absolute())
        return account->pw_dir;
    fail("cannot determine home directory; set HOME or XDG_CONFIG_HOME and XDG_DATA_HOME");
}
fs::path xdgDirectory(const char *variable, const char *fallback) {
    if (const char *value = std::getenv(variable); value && fs::path(value).is_absolute())
        return fs::path(value) / "blastirc";
    return homeDirectory() / fallback / "blastirc";
}
void privateDirectory(const fs::path &path) {
    std::error_code error;
    fs::create_directories(path, error);
    if (error)
        fail("create directory " + path.string() + ": " + error.message());
    if (::chmod(path.c_str(), 0700) != 0)
        systemFail("secure directory", path);
}
void closeChecked(int fd, const fs::path &path) {
    if (::close(fd) != 0)
        systemFail("close", path);
}
void atomicWrite(const fs::path &path, const string &content) {
    string pattern = path.string() + ".tmp.XXXXXX";
    vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    int fd = ::mkstemp(name.data());
    if (fd < 0)
        systemFail("create temporary file for", path);
    const fs::path temporary(name.data());
    bool renamed = false;
    try {
        if (::fchmod(fd, 0600) != 0)
            systemFail("secure", temporary);
        size_t offset = 0;
        while (offset < content.size()) {
            const auto length = ::write(fd, content.data() + offset, content.size() - offset);
            if (length < 0) {
                if (errno == EINTR)
                    continue;
                systemFail("write", temporary);
            }
            if (length == 0)
                fail("short write to " + temporary.string());
            offset += static_cast<size_t>(length);
        }
        if (::fsync(fd) != 0)
            systemFail("sync", temporary);
        const int toClose = fd;
        fd = -1;
        closeChecked(toClose, temporary);
        if (::rename(temporary.c_str(), path.c_str()) != 0)
            systemFail("replace", path);
        renamed = true;
        const int directory =
            ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory < 0)
            systemFail("open directory", path.parent_path());
        const int synced = ::fsync(directory);
        const int error = errno;
        ::close(directory);
        if (synced != 0) {
            errno = error;
            systemFail("sync directory", path.parent_path());
        }
    } catch (...) {
        if (fd >= 0)
            ::close(fd);
        if (!renamed)
            ::unlink(temporary.c_str());
        throw;
    }
}
void databaseCheck(sqlite3 *db, int code, const char *operation) {
    if (code != SQLITE_OK)
        fail(string(operation) + ": " + sqlite3_errmsg(db));
}
void execute(sqlite3 *db, const char *sql) {
    char *error = nullptr;
    const int result = sqlite3_exec(db, sql, nullptr, nullptr, &error);
    if (result != SQLITE_OK) {
        const string detail = error ? error : sqlite3_errmsg(db);
        sqlite3_free(error);
        fail("database: " + detail);
    }
}
struct Statement {
    sqlite3 *db;
    sqlite3_stmt *statement = nullptr;
    Statement(sqlite3 *connection, const char *sql) : db(connection) {
        databaseCheck(db, sqlite3_prepare_v2(db, sql, -1, &statement, nullptr),
                      "prepare database query");
    }
    ~Statement() { sqlite3_finalize(statement); }
    Statement(const Statement &) = delete;
    Statement &operator=(const Statement &) = delete;
    void bind(int index, const string &value) {
        if (value.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
            fail("database value is too large");
        databaseCheck(db,
                      sqlite3_bind_text(statement, index, value.data(),
                                        static_cast<int>(value.size()), SQLITE_TRANSIENT),
                      "bind text");
    }
    void bind(int index, int64_t value) {
        databaseCheck(db, sqlite3_bind_int64(statement, index, value), "bind integer");
    }
    void null(int index) { databaseCheck(db, sqlite3_bind_null(statement, index), "bind null"); }
    void reset() { databaseCheck(db, sqlite3_reset(statement), "reset database query"); }
    bool step() {
        const int result = sqlite3_step(statement);
        if (result == SQLITE_ROW)
            return true;
        if (result == SQLITE_DONE)
            return false;
        fail("database query: " + string(sqlite3_errmsg(db)));
    }
    string text(int index) const {
        const auto *data = sqlite3_column_text(statement, index);
        return data ? string(reinterpret_cast<const char *>(data),
                             sqlite3_column_bytes(statement, index))
                    : string{};
    }
    int64_t integer(int index) const { return sqlite3_column_int64(statement, index); }
};
std::optional<int64_t> localSequence(const string &id) {
    if (!id.starts_with("local:"))
        return {};
    int64_t sequence = 0;
    const char *start = id.data() + 6;
    const char *end = id.data() + id.size();
    const auto parsed = std::from_chars(start, end, sequence);
    if (parsed.ec != std::errc{} || parsed.ptr != end || sequence <= 0)
        return {};
    return sequence;
}
Message storedMessage(sqlite3 *db, const Statement &query) {
    Message message;
    message.storageSequence = query.integer(0);
    message.orderingTimestamp = query.integer(17);
    message.id = query.text(1);
    message.nick = query.text(2);
    message.text = query.text(3);
    message.time = query.text(4);
    message.reply = query.text(5);
    message.label = query.text(6);
    message.timestamp = query.integer(7);
    message.own = query.integer(8) != 0;
    message.pending = query.integer(9) != 0;
    message.deleted = query.integer(10) != 0;
    message.mention = query.integer(11) != 0;
    message.action = query.integer(12) != 0;
    message.presence = query.integer(13) != 0;
    message.activity = static_cast<ActivityKind>(query.integer(15));
    message.activityTarget = query.text(16);
    if (message.own) {
        if (query.integer(14) == 2)
            message.sendStatus = SendStatus::Sent;
        else if (query.integer(14) != 0)
            message.sendStatus = SendStatus::Unknown;
        else if (!message.presence)
            message.sendStatus = SendStatus::Confirmed;
    }
    message.history = true; // Restored rows never replay live animations or sounds.
    if (!message.deleted) {
        Statement reactions(
            db, "SELECT emoji,nick FROM reactions WHERE message=? ORDER BY emoji,ordinal");
        reactions.bind(1, query.integer(0));
        while (reactions.step())
            message.reactions[reactions.text(0)].push_back(reactions.text(1));
    }
    return message;
}
struct Transaction {
    sqlite3 *db;
    bool committed = false;
    explicit Transaction(sqlite3 *connection) : db(connection) { execute(db, "BEGIN IMMEDIATE"); }
    ~Transaction() {
        if (!committed)
            sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    void commit() {
        execute(db, "COMMIT");
        committed = true;
    }
};
[[noreturn]] void invalid(const string &key, const string &expectation) {
    fail("invalid configuration '" + key + "': expected " + expectation);
}
const toml::table *table(const toml::table &parent, const char *key) {
    const auto *node = parent.get(key);
    if (!node)
        return nullptr;
    const auto *result = node->as_table();
    if (!result)
        invalid(key, "a table");
    return result;
}
template <class T> void field(const toml::table &source, const char *key, T &destination) {
    if (const auto *node = source.get(key)) {
        const auto value = node->value<T>();
        if (!value)
            invalid(key, "a value of the correct type");
        destination = *value;
    }
}
template <class T> void exactField(const toml::table &source, const char *key, T &destination) {
    if (const auto *node = source.get(key)) {
        const bool correct = std::is_same_v<T, string> ? node->is_string()
                             : std::is_same_v<T, bool> ? node->is_boolean()
                                                       : node->is_integer();
        if (!correct)
            invalid(key, "a value of the exact TOML type");
        field(source, key, destination);
    }
}
vector<string> stringArray(const toml::node &node, const string &key) {
    const auto *values = node.as_array();
    if (!values)
        invalid(key, "an array of strings");
    vector<string> result;
    for (const auto &value : *values) {
        const auto text = value.value<string>();
        if (!text)
            invalid(key, "an array of strings");
        result.push_back(*text);
    }
    return result;
}
void stringMap(const toml::table &source, const char *key, map<string, string> &destination,
               const char *excluded = nullptr, const char *excludedAlso = nullptr) {
    if (const auto *values = table(source, key)) {
        for (const auto &[name, value] : *values) {
            if ((excluded && name.str() == excluded) ||
                (excludedAlso && name.str() == excludedAlso))
                continue;
            auto text = value.value<string>();
            if (!text)
                invalid(string(key) + "." + string(name.str()), "a string");
            destination[string(name.str())] = *text;
        }
    }
}
void schemaVersion(const toml::table &source) {
    int version = 1;
    field(source, "schema_version", version);
    if (version != 1)
        invalid("schema_version", "1 (configuration was not modified)");
}
string peopleModeName(SidebarMode mode) {
    const auto token = sidebarModeToken(mode);
    if (!token.empty())
        return string(token);
    invalid("workspace.people_mode", "expanded, compact, or hidden");
}
PresenceMode presenceMode(const string &text, const string &key, bool channel = false) {
    if (text == "all")
        return PresenceMode::All;
    if (text == "smart")
        return PresenceMode::Smart;
    if (text == "hidden")
        return PresenceMode::Hidden;
    if (channel && text == "global")
        return PresenceMode::Global;
    invalid(key, channel ? "all, smart, hidden, or global" : "all, smart, or hidden");
}
string presenceName(PresenceMode value) {
    switch (value) {
    case PresenceMode::All:
        return "all";
    case PresenceMode::Smart:
        return "smart";
    case PresenceMode::Hidden:
        return "hidden";
    case PresenceMode::Global:
        return "global";
    }
    invalid("presence", "a valid presence mode");
}
string fxName(FxLevel value) {
    switch (value) {
    case FxLevel::Off:
        return "off";
    case FxLevel::Subtle:
        return "subtle";
    case FxLevel::Full:
        return "full";
    }
    invalid("fx", "a valid FX level");
}
string sendMotionName(SendMotion value) {
    switch (value) {
    case SendMotion::None:
        return "none";
    case SendMotion::Flight:
        return "flight";
    case SendMotion::Rise:
        return "rise";
    case SendMotion::Pop:
        return "pop";
    case SendMotion::Slide:
        return "slide";
    case SendMotion::Drift:
        return "drift";
    }
    invalid("appearance.send_motion", "none, flight, rise, pop, slide, or drift");
}
string sendRevealName(SendReveal value) {
    switch (value) {
    case SendReveal::None:
        return "none";
    case SendReveal::Fade:
        return "fade";
    case SendReveal::Pixels:
        return "pixels";
    }
    invalid("appearance.send_reveal", "none, fade, or pixels");
}
SendMotion motionValue(const string &token, const char *key, bool incoming = false) {
    for (auto motion : {SendMotion::None, SendMotion::Flight, SendMotion::Rise, SendMotion::Pop,
                        SendMotion::Slide, SendMotion::Drift})
        if (token == sendMotionName(motion) && (!incoming || motion != SendMotion::Flight))
            return motion;
    invalid(key, incoming ? "none, rise, pop, slide, or drift"
                          : "none, flight, rise, pop, slide, or drift");
}
SendReveal revealValue(const string &token, const char *key) {
    for (auto reveal : {SendReveal::None, SendReveal::Fade, SendReveal::Pixels})
        if (token == sendRevealName(reveal))
            return reveal;
    invalid(key, "none, fade, or pixels");
}
string foolDisplayName(FoolDisplay value) {
    switch (value) {
    case FoolDisplay::Minimized:
        return "minimized";
    case FoolDisplay::Blurred:
        return "blurred";
    case FoolDisplay::Dimmed:
        return "dimmed";
    case FoolDisplay::Hidden:
        return "hidden";
    case FoolDisplay::Shown:
        // Fully shown is a per-buffer effective policy, never a global preference.
        break;
    }
    invalid("appearance.fool_display", "minimized, blurred, dimmed, or hidden");
}
string previewModeName(PreviewMode value) {
    switch (value) {
    case PreviewMode::Off:
        return "off";
    case PreviewMode::Click:
        return "click";
    case PreviewMode::Automatic:
        return "automatic";
    }
    invalid("previews", "off, click, or automatic");
}
void previewModeField(const toml::table &row, const char *key, PreviewMode &value) {
    string token = previewModeName(value);
    exactField(row, key, token);
    for (auto mode : {PreviewMode::Off, PreviewMode::Click, PreviewMode::Automatic})
        if (token == previewModeName(mode)) {
            value = mode;
            return;
        }
    invalid("previews." + string(key), "off, click, or automatic");
}
Settings defaults() {
    Settings settings;
    for (const char *cue :
         {"kick",   "ban",    "netsplit", "mention",    "reaction",  "redact", "join",
          "part",   "quit",   "unread",   "disconnect", "send",      "echo",   "message",
          "action", "notice", "boom",     "rumble",     "ding",      "blip",   "tick",
          "whoosh", "click",  "pop",      "hush",       "power_down"})
        settings.cues.emplace(cue, Cue{});
    settings.bindings = {{"switch-buffer", "C-x b"},
                         {"execute-command", "M-x"},
                         {"jump-channel", "C-x j"},
                         {"conversation-back", "C-o"},
                         {"conversation-forward", "C-i"},
                         {"cancel", "C-g"},
                         {"search", "C-s"},
                         {"settings", "C-,"},
                         {"toggle-people", ""},
                         {"toggle-navigation", ""},
                         {"toggle-fools", "C-x f"},
                         {"show-fools", ""},
                         {"hide-fools", ""},
                         {"global-mentions", ""},
                         {"send", "RET"},
                         {"newline", "S-RET"},
                         {"message-prev", "C-p"},
                         {"message-next", "C-n"},
                         {"scroll-up", "PRIOR"},
                         {"scroll-down", "NEXT"},
                         {"message-reply", "C-r"},
                         {"message-react", "C-e"},
                         {"beginning-of-line", "C-a"},
                         {"end-of-line", ""},
                         {"backward-char", "C-b"},
                         {"forward-char", "C-f"},
                         {"beginning-of-buffer", "M-<"},
                         {"end-of-buffer", "M->"},
                         {"kill-word", "M-d"},
                         {"backward-kill-word", "M-BACKSPACE"},
                         {"previous-line", ""},
                         {"next-line", ""},
                         {"backward-word", "M-b"},
                         {"forward-word", "M-f"},
                         {"delete-char", "C-d"},
                         {"backward-delete", "BACKSPACE"},
                         {"kill-line", "C-k"},
                         {"backward-kill-line", ""},
                         {"transpose-chars", "C-t"},
                         {"kill-region", "C-w"},
                         {"copy-region", "M-w"},
                         {"yank", "C-y"},
                         {"cycle-spelling", "C-;"},
                         {"undo", "C-/"},
                         {"redo", ""},
                         {"copy", ""},
                         {"cut", ""},
                         {"paste", ""},
                         {"select-all", ""},
                         {"set-mark", "C-SPC"},
                         {"history-previous", "M-p"},
                         {"history-next", "M-n"},
                         {"complete", "TAB"}};
    return settings;
}
void range(float value, float low, float high, const char *name) {
    if (!std::isfinite(value) || value < low || value > high)
        invalid(name,
                "a finite number in [" + std::to_string(low) + ", " + std::to_string(high) + "]");
}
float cursorValue(double value, double low, double high, float fallback) {
    return std::isfinite(value) ? static_cast<float>(std::clamp(value, low, high)) : fallback;
}
void validate(const Settings &settings) {
    if (const auto error = validateUploadConfig(settings.uploads); !error.empty())
        invalid("uploads", error);
    if (!safeUploadUrl(settings.uploads.customEndpoint))
        invalid("uploads.custom_endpoint", "an HTTPS URL without credentials or controls");
    if (settings.uploads.password.find('\0') != string::npos ||
        settings.uploads.password.size() > 16384)
        invalid("uploads.password", "a password without NUL, at most 16384 bytes");
    if (settings.uploads.maxBytes % (1024 * 1024) != 0)
        invalid("uploads.size_limit_mib", "a whole number of MiB");
    fxName(settings.fx);
    sendMotionName(settings.sendMotion);
    sendRevealName(settings.sendReveal);
    motionValue(sendMotionName(settings.incomingMotion), "appearance.incoming_motion", true);
    sendRevealName(settings.incomingReveal);
    range(settings.incomingDurationMs, 100, 2000, "appearance.incoming_duration_ms");
    foolDisplayName(settings.foolDisplay);
    if (settings.messageWrapColumns < 0 || settings.messageWrapColumns > 1000)
        invalid("appearance.message_wrap_columns", "an integer from 0 through 1000");
    previewModeName(settings.linkPreviews);
    previewModeName(settings.imagePreviews);
    previewModeName(settings.mediaPreviews);
    if (settings.previewMaxHeight < 64 || settings.previewMaxHeight > 240)
        invalid("previews.max_height", "an integer from 64 through 240");
    if (settings.previewMediaMaxMiB < 1 || settings.previewMediaMaxMiB > 512)
        invalid("previews.media_max_mib", "an integer from 1 through 512");
    if (settings.previewMediaMaxSeconds < 1 || settings.previewMediaMaxSeconds > 7200)
        invalid("previews.media_max_seconds", "an integer from 1 through 7200");
    peopleModeName(settings.peopleMode);
    if (settings.presence == PresenceMode::Global)
        invalid("presence.mode", "all, smart, or hidden");
    presenceName(settings.presence);
    const auto asciiAlnum = [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    };
    const auto &language = settings.spellcheckLanguage;
    if (language.empty() || !asciiAlnum(language.front()) ||
        !std::all_of(language.begin(), language.end(),
                     [&](unsigned char c) { return asciiAlnum(c) || c == '_' || c == '-'; }))
        invalid("composer.spellcheck_language",
                "auto or a dictionary basename (letters, digits, _ or -)");
    if (settings.cursorShape != "line" && settings.cursorShape != "box" &&
        settings.cursorShape != "underline")
        invalid("cursor.shape", "line, box, or underline");
    if (settings.cursorEffect != "off" && settings.cursorEffect != "trail" &&
        settings.cursorEffect != "comet" && settings.cursorEffect != "ribbon" &&
        settings.cursorEffect != "pulse")
        invalid("cursor.effect", "off, trail, comet, ribbon, or pulse");
    range(settings.opacity, 0, 1, "appearance.opacity");
    range(settings.scale, .7f, 2.f, "appearance.scale");
    range(settings.messageScale, .7f, 2.f, "appearance.message_scale");
    for (const auto &[weight, name] :
         {std::pair{settings.uiFontWeight, "appearance.ui_font_weight"},
          std::pair{settings.messageFontWeight, "appearance.message_font_weight"}})
        if (weight != 300 && weight != 400 && weight != 500 && weight != 600 && weight != 700)
            invalid(name, "300, 400, 500, 600, or 700");
    range(settings.masterVolume, 0, 1, "audio.master_volume");
    range(settings.gravity, -10000, 10000, "physics.gravity");
    range(settings.drag, 0, 1000, "physics.drag");
    range(settings.shake, 0, 100, "physics.shake");
    if (settings.retentionDays < 0 || settings.retentionDays > 365000)
        invalid("history.retention_days", "an integer from 0 through 365000");
    if (settings.maxMessages < 1)
        invalid("history.max_messages", "a positive integer");
    for (const auto &[name, cue] : settings.cues) {
        range(cue.volume, 0, 1, ("audio.cues." + name + ".volume").c_str());
        range(cue.pitch, .05f, 8, ("audio.cues." + name + ".pitch").c_str());
    }
    for (const auto &[name, mode] : settings.channelPresence) {
        (void)name;
        presenceName(mode);
    }
    if (settings.nickPalette != "THEME" && settings.nickPalette != "CLASSIC" &&
        settings.nickPalette != "VIVID")
        invalid("appearance.nick_palette", "THEME, CLASSIC, or VIVID");
    if (settings.avatarStyle != "sprite" && settings.avatarStyle != "monogram" &&
        settings.avatarStyle != "initials" && settings.avatarStyle != "none")
        invalid("appearance.avatar_style", "sprite, monogram, initials, or none");
    if (settings.themeSource != "auto" && settings.themeSource != "builtin" &&
        settings.themeSource != "noctalia")
        invalid("appearance.theme_source", "auto, builtin, or noctalia");
    if (settings.pitchVariation != "natural" && settings.pitchVariation != "musical" &&
        settings.pitchVariation != "fixed")
        invalid("audio.pitch_variation", "natural, musical, or fixed");
}
bool profileToken(std::string_view value) {
    return !value.empty() && value.front() != ':' &&
           std::none_of(value.begin(), value.end(),
                        [](unsigned char c) { return c <= 32 || c == 127; });
}
bool profileFraming(std::string_view value) {
    return value.find_first_of(std::string_view("\r\n\0", 3)) != std::string_view::npos;
}
bool profileTrailing(std::string_view value) {
    return value.empty() || value.front() == ':' || value.find(' ') != std::string_view::npos;
}
std::string_view identityFieldsError(const Identity &identity) {
    if (identity.name.empty())
        return "Identity profile names are required.";
    if (!irc::validNickname(identity.nick))
        return "Choose a nickname without spaces, controls or IRC separators (maximum 505 bytes).";
    if (!profileToken(identity.username) || identity.username.size() > 512)
        return "Username must be a nonempty IRC token without spaces or controls.";
    if (profileFraming(identity.realname) || identity.realname.size() > 4096)
        return "Real name cannot contain NUL or line breaks.";
    if (10 + identity.username.size() + identity.realname.size() +
            (profileTrailing(identity.realname) ? 1 : 0) >
        510)
        return "Username and real name are too long for the initial IRC USER message.";
    if (identity.password.size() > 16384 || identity.password.find('\0') != string::npos)
        return "The SASL password cannot contain NUL or exceed 16384 bytes.";
    if (identity.certificate.find('\0') != string::npos || identity.key.find('\0') != string::npos)
        return "Certificate and private-key file paths cannot contain NUL.";
    return {};
}
std::string_view serverFieldsError(const ServerConfig &server, const Identity *identity) {
    if (server.name.empty() || (server.host.empty() && server.sojuParent.empty()))
        return "Network name and address are required.";
    if (server.host.size() > 253 ||
        std::any_of(server.host.begin(), server.host.end(),
                    [](unsigned char c) { return c <= 32 || c == 127; }))
        return "The server address cannot contain spaces or controls, or exceed 253 bytes.";
    if (server.port < 1 || server.port > 65535)
        return "Port must be between 1 and 65535.";
    if (!identity)
        return "Choose an existing identity for this network.";
    if (server.sojuAccount && identity->username.find('/') != string::npos)
        return "A soju control account requires a base username without a /network selector.";
    if (server.sasl != "" && server.sasl != "PLAIN" && server.sasl != "EXTERNAL")
        return "server.sasl must be empty, PLAIN, or EXTERNAL.";
    if (server.sasl == "PLAIN" && !server.tls)
        return "SASL PLAIN requires TLS to keep your password private.";
    if (server.sasl == "EXTERNAL" && (!server.tls || identity->certificate.empty()))
        return "EXTERNAL requires TLS and a certificate; its PEM may also contain the private key.";
    if (profileFraming(server.password) || server.password.size() > 16384)
        return "The server password cannot contain NUL or line breaks.";
    if (!server.password.empty() &&
        5 + server.password.size() + (profileTrailing(server.password) ? 1 : 0) > 510)
        return "The server password is too long for the initial IRC PASS message.";
    const std::string_view bouncer = server.bouncer;
    if (bouncer != "none" && bouncer != "soju" && bouncer != "znc" &&
        !(bouncer.starts_with("soju:") && bouncer.size() <= 5 + 497 &&
          profileToken(bouncer.substr(5))))
        return "server.bouncer must be none, soju, znc, or a soju network binding.";
    if (server.sojuAccount && (server.bouncer != "soju" || !server.sojuParent.empty()))
        return "A soju account must be an unbound soju control profile.";
    if (server.sojuParent.empty() != server.sojuNetworkId.empty())
        return "A soju child requires both parent profile and remote network ID.";
    if (!server.sojuNetworkId.empty() && !profileToken(server.sojuNetworkId))
        return "A soju remote network ID must be an IRC token.";
    if (!server.sojuParent.empty() &&
        (server.bouncer != "soju:" + server.sojuNetworkId || !server.password.empty()))
        return "Soju children must bind their remote ID and inherit credentials without stored "
               "passwords.";
    if (!server.sojuTrustedFileHost.empty() &&
        (server.sojuTrustedFileHost.find_first_not_of(
             "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") != string::npos ||
         server.sojuTrustedFileHost.front() == '.' || server.sojuTrustedFileHost.back() == '.'))
        return "Trusted file host must be one exact hostname without scheme, path or port.";
    for (const auto &channel : server.channels)
        if (profileFraming(channel))
            return "Auto-join channels cannot contain NUL or line breaks.";
    return {};
}
void validateProfiles(const Model &model) {
    std::map<std::string_view, const Identity *> identities;
    set<std::string_view> servers;
    for (const auto &identity : model.identities) {
        if (identity.name.empty() || !identities.emplace(identity.name, &identity).second)
            invalid("identity.name", "a nonempty unique profile name");
        if (const auto error = identityFieldsError(identity); !error.empty())
            fail("invalid configuration 'identity': " + string(error));
    }
    for (const auto &server : model.servers) {
        if (server.name.empty() || !servers.insert(server.name).second)
            invalid("server.name", "a nonempty unique network name");
        const ServerConfig *endpoint = &server;
        if (!server.sojuParent.empty()) {
            const auto parent =
                std::find_if(model.servers.begin(), model.servers.end(), [&](const auto &entry) {
                    return entry.name == server.sojuParent && entry.sojuAccount;
                });
            if (parent == model.servers.end())
                invalid("server.soju_parent", "an existing soju account profile");
            endpoint = &*parent;
        }
        const auto identity = identities.find(endpoint->identity);
        if (identity == identities.end())
            invalid("server.identity", "the name of an existing identity profile");
        if (const auto error = serverFieldsError(server, identity->second); !error.empty())
            fail("invalid configuration 'server': " + string(error));
        if (!server.sojuParent.empty()) {
            const auto parent =
                std::find_if(model.servers.begin(), model.servers.end(), [&](const auto &entry) {
                    return entry.name == server.sojuParent && entry.sojuAccount;
                });
            if (parent == model.servers.end())
                invalid("server.soju_parent", "an existing soju account profile");
            if (server.name == server.sojuParent)
                invalid("server.soju_parent", "a different parent profile");
            for (const auto &other : model.servers)
                if (&other != &server && other.sojuParent == server.sojuParent &&
                    other.sojuNetworkId == server.sojuNetworkId)
                    invalid("server.soju_network_id", "a unique child association per account");
        }
    }
}
void parseProfiles(const toml::table &document, Model &model) {
    schemaVersion(document);
    auto readRows = [&](const char *key, auto &&visitor) {
        if (const auto *node = document.get(key)) {
            const auto *rows = node->as_array();
            if (!rows)
                invalid(key, "an array of tables");
            for (const auto &row : *rows) {
                const auto *entry = row.as_table();
                if (!entry)
                    invalid(key, "an array of tables");
                visitor(*entry);
            }
        }
    };
    readRows("identity", [&](const toml::table &row) {
        Identity identity;
        field(row, "name", identity.name);
        field(row, "nick", identity.nick);
        field(row, "username", identity.username);
        field(row, "realname", identity.realname);
        field(row, "password", identity.password);
        field(row, "certificate", identity.certificate);
        field(row, "key", identity.key);
        model.identities.push_back(std::move(identity));
    });
    if (model.identities.empty())
        model.identities.emplace_back();
    readRows("server", [&](const toml::table &row) {
        ServerConfig server;
        field(row, "name", server.name);
        field(row, "alias", server.alias);
        field(row, "avatar", server.avatar);
        field(row, "address", server.host);
        field(row, "port", server.port);
        field(row, "tls", server.tls);
        field(row, "identity", server.identity);
        field(row, "sasl", server.sasl);
        field(row, "password", server.password);
        field(row, "bouncer", server.bouncer);
        exactField(row, "soju_account", server.sojuAccount);
        exactField(row, "soju_parent", server.sojuParent);
        exactField(row, "soju_network_id", server.sojuNetworkId);
        exactField(row, "soju_enabled", server.sojuEnabled);
        exactField(row, "soju_trusted_file_host", server.sojuTrustedFileHost);
        if (const auto *channels = row.get("channels"))
            server.channels = stringArray(*channels, "server.channels");
        model.servers.push_back(std::move(server));
    });
    validateProfiles(model);
}
void parseSettings(const toml::table &document, Settings &settings) {
    schemaVersion(document);
    if (const auto *row = table(document, "uploads")) {
        auto &uploads = settings.uploads;
        string provider(uploadProviderToken(uploads.provider));
        exactField(*row, "provider", provider);
        const auto parsed = parseUploadProvider(provider);
        if (!parsed)
            invalid("uploads.provider", "a supported lowercase upload provider");
        uploads.provider = *parsed;
        exactField(*row, "custom_endpoint", uploads.customEndpoint);
        exactField(*row, "expiry_hours", uploads.expiryHours);
        exactField(*row, "litterbox_expiry", uploads.litterboxExpiry);
        exactField(*row, "secret", uploads.secret);
        exactField(*row, "username", uploads.username);
        exactField(*row, "password", uploads.password);
        int64_t limit = static_cast<int64_t>(uploads.maxBytes / (1024 * 1024));
        exactField(*row, "size_limit_mib", limit);
        if (limit < 1 || limit > 1024 * 1024)
            invalid("uploads.size_limit_mib", "an integer from 1 through 1048576");
        uploads.maxBytes = static_cast<uint64_t>(limit) * 1024 * 1024;
    }
    // Existing preference files retain the legacy implicit level until explicitly changed.
    settings.fx = FxLevel::Full;
    if (const auto *row = table(document, "appearance")) {
        string fx = fxName(settings.fx);
        field(*row, "fx", fx);
        if (fx == "off")
            settings.fx = FxLevel::Off;
        else if (fx == "subtle")
            settings.fx = FxLevel::Subtle;
        else if (fx == "full")
            settings.fx = FxLevel::Full;
        else
            invalid("appearance.fx", "off, subtle, or full");
        string movement = sendMotionName(settings.sendMotion);
        exactField(*row, "send_motion", movement);
        settings.sendMotion = motionValue(movement, "appearance.send_motion");
        string reveal = sendRevealName(settings.sendReveal);
        exactField(*row, "send_reveal", reveal);
        settings.sendReveal = revealValue(reveal, "appearance.send_reveal");
        movement = sendMotionName(settings.incomingMotion);
        exactField(*row, "incoming_motion", movement);
        settings.incomingMotion = motionValue(movement, "appearance.incoming_motion", true);
        reveal = sendRevealName(settings.incomingReveal);
        exactField(*row, "incoming_reveal", reveal);
        settings.incomingReveal = revealValue(reveal, "appearance.incoming_reveal");
        field(*row, "incoming_duration_ms", settings.incomingDurationMs);
        string fools = foolDisplayName(settings.foolDisplay);
        exactField(*row, "fool_display", fools);
        bool foundFoolDisplay = false;
        for (auto style : {FoolDisplay::Minimized, FoolDisplay::Blurred, FoolDisplay::Dimmed,
                           FoolDisplay::Hidden})
            if (fools == foolDisplayName(style)) {
                settings.foolDisplay = style;
                foundFoolDisplay = true;
                break;
            }
        if (!foundFoolDisplay)
            invalid("appearance.fool_display", "minimized, blurred, dimmed, or hidden");
        exactField(*row, "fool_unread", settings.foolUnread);
        exactField(*row, "message_wrap_columns", settings.messageWrapColumns);
        field(*row, "opacity", settings.opacity);
        field(*row, "transparency", settings.transparency);
        field(*row, "scale", settings.scale);
        field(*row, "message_scale", settings.messageScale);
        field(*row, "ui_font", settings.uiFont);
        field(*row, "message_font", settings.messageFont);
        exactField(*row, "ui_font_weight", settings.uiFontWeight);
        exactField(*row, "message_font_weight", settings.messageFontWeight);
        field(*row, "timestamps", settings.timestamps);
        field(*row, "avatars", settings.avatars);
        field(*row, "avatar_style", settings.avatarStyle);
        field(*row, "theme_source", settings.themeSource);
        field(*row, "nick_palette", settings.nickPalette);
        field(*row, "reduced_motion", settings.reducedMotion);
    }
    if (const auto *row = table(document, "previews")) {
        previewModeField(*row, "links", settings.linkPreviews);
        previewModeField(*row, "images", settings.imagePreviews);
        previewModeField(*row, "media", settings.mediaPreviews);
        exactField(*row, "max_height", settings.previewMaxHeight);
        exactField(*row, "media_max_mib", settings.previewMediaMaxMiB);
        exactField(*row, "media_max_seconds", settings.previewMediaMaxSeconds);
        exactField(*row, "allow_local", settings.previewAllowLocal);
    }
    if (const auto *row = table(document, "presentation")) {
        if (row->contains("message_layouts")) {
            const auto *layouts = table(*row, "message_layouts");
            for (const auto &[name, value] : *layouts) {
                const auto token = value.value<string>();
                const string key = "presentation.message_layouts." + string(name.str());
                if (!token || (*token != "bubbles" && *token != "irc" && *token != "stacked"))
                    invalid(key, "bubbles, irc, or stacked");
                settings.messageLayouts[string(name.str())] = *token == "irc" ? MessageLayout::Irc
                                                              : *token == "stacked"
                                                                  ? MessageLayout::Stacked
                                                                  : MessageLayout::Bubbles;
            }
        } else if (const auto *channels = row->get("irc_channels")) {
            for (const auto &id : stringArray(*channels, "presentation.irc_channels"))
                settings.messageLayouts[id] = MessageLayout::Irc;
        }
        if (const auto *pins = row->get("pinned_buffers")) {
            for (const auto &id : stringArray(*pins, "presentation.pinned_buffers"))
                if (std::find(settings.pinnedBuffers.begin(), settings.pinnedBuffers.end(), id) ==
                    settings.pinnedBuffers.end())
                    settings.pinnedBuffers.push_back(id);
        }
    }
    if (const auto *row = table(document, "composer")) {
        exactField(*row, "spellcheck", settings.spellcheck);
        exactField(*row, "spellcheck_language", settings.spellcheckLanguage);
        exactField(*row, "word_prediction", settings.wordPrediction);
    }
    if (const auto *row = table(document, "cursor")) {
        field(*row, "shape", settings.cursorShape);
        field(*row, "effect", settings.cursorEffect);
        double duration = settings.cursorTrailDurationMs, strength = settings.cursorTrailStrength;
        field(*row, "trail_duration_ms", duration);
        field(*row, "trail_strength", strength);
        settings.cursorTrailDurationMs = cursorValue(duration, 40, 240, 120.f);
        settings.cursorTrailStrength = cursorValue(strength, .05, .35, .20f);
    }
    if (const auto *row = table(document, "audio")) {
        field(*row, "enabled", settings.sound);
        field(*row, "master_volume", settings.masterVolume);
        field(*row, "pitch_variation", settings.pitchVariation);
        if (const auto *cues = table(*row, "cues")) {
            for (const auto &[name, value] : *cues) {
                const auto *cue = value.as_table();
                if (!cue)
                    invalid("audio.cues." + string(name.str()), "a table");
                Cue config;
                field(*cue, "enabled", config.enabled);
                field(*cue, "volume", config.volume);
                field(*cue, "pitch", config.pitch);
                settings.cues[string(name.str())] = config;
            }
        }
    }
    if (const auto *row = table(document, "presence")) {
        string mode = presenceName(settings.presence);
        field(*row, "mode", mode);
        settings.presence = presenceMode(mode, "presence.mode");
        if (const auto *channels = table(*row, "channels"))
            for (const auto &[name, value] : *channels) {
                const auto text = value.value<string>();
                if (!text)
                    invalid("presence.channels." + string(name.str()), "a presence mode string");
                settings.channelPresence[string(name.str())] =
                    presenceMode(*text, "presence.channels." + string(name.str()), true);
            }
    }
    if (const auto *row = table(document, "typing")) {
        field(*row, "send", settings.typingSend);
        field(*row, "receive", settings.typingReceive);
    }
    if (const auto *row = table(document, "physics")) {
        field(*row, "gravity", settings.gravity);
        field(*row, "drag", settings.drag);
        field(*row, "shake", settings.shake);
    }
    if (const auto *row = table(document, "history")) {
        field(*row, "retention_days", settings.retentionDays);
        field(*row, "max_messages", settings.maxMessages);
    }
    stringMap(document, "bindings", settings.bindings);
    for (const char *obsolete :
         {"split-below", "split-right", "close-pane", "only-pane", "other-pane", "reset-layout",
          "float-pane", "dock-pane", "new-tab", "focus-conversation", "restore-workspace",
          "close-tab", "next-tab", "style-modern", "style-glass", "style-hacker"})
        settings.bindings.erase(obsolete);
    std::erase_if(settings.bindings, [](const auto &entry) {
        return entry.first.starts_with("aux-") || entry.first.starts_with("split-buffer:") ||
               entry.first.starts_with("reorder-tab:") || entry.first.starts_with("select-tab:") ||
               entry.first.starts_with("select-tab-keyboard:") ||
               entry.first.starts_with("close-tab:");
    });
    const auto *savedBindings = table(document, "bindings");
    if ((!savedBindings || !savedBindings->contains("jump-channel")) &&
        std::any_of(settings.bindings.begin(), settings.bindings.end(), [](const auto &entry) {
            return entry.first != "jump-channel" &&
                   ui_logic::bindingConflicts("C-x j", entry.second);
        }))
        settings.bindings.at("jump-channel").clear();
    ui_logic::migrateMessageBindings(settings.bindings, [&](std::string_view name) {
        return savedBindings && savedBindings->contains(name);
    });
    if ((!savedBindings || !savedBindings->contains("transpose-chars")) &&
        std::any_of(settings.bindings.begin(), settings.bindings.end(), [](const auto &entry) {
            return entry.first != "transpose-chars" &&
                   ui_logic::bindingConflicts("C-t", entry.second);
        }))
        settings.bindings.at("transpose-chars").clear();
    for (const char *name : {"conversation-back", "conversation-forward", "cycle-spelling"}) {
        if (savedBindings && savedBindings->contains(name))
            continue;
        auto &binding = settings.bindings.at(name);
        if (std::any_of(settings.bindings.begin(), settings.bindings.end(), [&](const auto &entry) {
                return entry.first != name && ui_logic::bindingConflicts(binding, entry.second);
            }))
            binding.clear();
    }
    stringMap(document, "nick_colors", settings.nickColors);
    if (const auto *row = table(document, "workspace")) {
        if (const auto *node = row->get("people_mode")) {
            const auto mode = node->value<string>();
            if (!mode || (*mode != "expanded" && *mode != "compact" && *mode != "hidden"))
                invalid("workspace.people_mode", "expanded, compact, or hidden");
            settings.peopleMode = mode == "expanded"  ? SidebarMode::Expanded
                                  : mode == "compact" ? SidebarMode::Compact
                                                      : SidebarMode::Hidden;
        } else {
            bool visible = false;
            if (const auto *node = row->get("people_visible"); node && !node->is_boolean())
                invalid("workspace.people_visible", "a boolean");
            field(*row, "people_visible", visible);
            settings.peopleMode = visible ? SidebarMode::Expanded : SidebarMode::Hidden;
        }
    }
    stringMap(document, "workspace", settings.workspace, "people_visible", "people_mode");
    if (const auto *list = document.get("friends")) {
        const auto names = stringArray(*list, "friends");
        settings.friends = set<string>(names.begin(), names.end());
    }
    if (const auto *node = document.get("fools")) {
        if (const auto *row = node->as_table()) {
            if (const auto *list = row->get("names")) {
                const auto names = stringArray(*list, "fools.names");
                settings.fools = set<string>(names.begin(), names.end());
            }
            if (const auto *buffers = table(*row, "buffers"))
                for (const auto &[name, value] : *buffers) {
                    const auto shown = value.value<bool>();
                    if (!shown)
                        invalid("fools.buffers." + string(name.str()), "a boolean");
                    settings.bufferFools[string(name.str())] = *shown;
                }
        } else {
            const auto names = stringArray(*node, "fools");
            settings.fools = set<string>(names.begin(), names.end());
        }
    }
    validate(settings);
}
toml::array array(const auto &values) {
    toml::array result;
    for (const auto &value : values)
        result.push_back(value);
    return result;
}
toml::table mapping(const map<string, string> &values) {
    toml::table result;
    for (const auto &[key, value] : values)
        result.insert(key, value);
    return result;
}
string formatted(const toml::table &document) {
    std::ostringstream output;
    output << document;
    return output.str() + "\n";
}
toml::table profileDocument(const Model &model) {
    toml::array identities, servers;
    for (const auto &identity : model.identities)
        identities.push_back(toml::table{{"name", identity.name},
                                         {"nick", identity.nick},
                                         {"username", identity.username},
                                         {"realname", identity.realname},
                                         {"password", identity.password},
                                         {"certificate", identity.certificate},
                                         {"key", identity.key}});
    for (const auto &server : model.servers)
        servers.push_back(toml::table{{"name", server.name},
                                      {"alias", server.alias},
                                      {"avatar", server.avatar},
                                      {"address", server.host},
                                      {"port", server.port},
                                      {"tls", server.tls},
                                      {"identity", server.identity},
                                      {"sasl", server.sasl},
                                      {"password", server.password},
                                      {"bouncer", server.bouncer},
                                      {"soju_account", server.sojuAccount},
                                      {"soju_parent", server.sojuParent},
                                      {"soju_network_id", server.sojuNetworkId},
                                      {"soju_enabled", server.sojuEnabled},
                                      {"soju_trusted_file_host", server.sojuTrustedFileHost},
                                      {"channels", array(server.channels)}});
    return toml::table{
        {"schema_version", 1}, {"identity", std::move(identities)}, {"server", std::move(servers)}};
}
toml::table settingsDocument(const Settings &settings) {
    toml::table cues, channels, bufferFools, messageLayouts;
    for (const auto &[name, cue] : settings.cues)
        cues.insert(
            name,
            toml::table{{"enabled", cue.enabled}, {"volume", cue.volume}, {"pitch", cue.pitch}});
    for (const auto &[name, mode] : settings.channelPresence)
        channels.insert(name, presenceName(mode));
    for (const auto &[name, shown] : settings.bufferFools)
        bufferFools.insert(name, shown);
    for (const auto &[name, layout] : settings.messageLayouts) {
        switch (layout) {
        case MessageLayout::Bubbles:
            messageLayouts.insert(name, "bubbles");
            break;
        case MessageLayout::Irc:
            messageLayouts.insert(name, "irc");
            break;
        case MessageLayout::Stacked:
            messageLayouts.insert(name, "stacked");
            break;
        default:
            invalid("presentation.message_layouts." + name, "a valid message layout");
        }
    }
    auto bindings = mapping(settings.bindings);
    for (const char *obsolete : {"style-modern", "style-glass", "style-hacker"})
        bindings.erase(obsolete);
    auto workspace = mapping(settings.workspace);
    workspace.erase("people_visible");
    workspace.insert_or_assign("people_mode", peopleModeName(settings.peopleMode));
    return toml::table{
        {"schema_version", 1},
        {"uploads",
         toml::table{
             {"provider", string(uploadProviderToken(settings.uploads.provider))},
             {"custom_endpoint", settings.uploads.customEndpoint},
             {"expiry_hours", settings.uploads.expiryHours},
             {"litterbox_expiry", settings.uploads.litterboxExpiry},
             {"secret", settings.uploads.secret},
             {"size_limit_mib", static_cast<int64_t>(settings.uploads.maxBytes / (1024 * 1024))},
             {"username", settings.uploads.username},
             {"password", settings.uploads.password}}},
        {"friends", array(settings.friends)},
        {"fools",
         toml::table{{"names", array(settings.fools)}, {"buffers", std::move(bufferFools)}}},
        {"appearance", toml::table{{"fx", fxName(settings.fx)},
                                   {"send_motion", sendMotionName(settings.sendMotion)},
                                   {"send_reveal", sendRevealName(settings.sendReveal)},
                                   {"incoming_motion", sendMotionName(settings.incomingMotion)},
                                   {"incoming_reveal", sendRevealName(settings.incomingReveal)},
                                   {"incoming_duration_ms", settings.incomingDurationMs},
                                   {"fool_display", foolDisplayName(settings.foolDisplay)},
                                   {"fool_unread", settings.foolUnread},
                                   {"message_wrap_columns", settings.messageWrapColumns},
                                   {"opacity", settings.opacity},
                                   {"transparency", settings.transparency},
                                   {"scale", settings.scale},
                                   {"message_scale", settings.messageScale},
                                   {"ui_font", settings.uiFont},
                                   {"message_font", settings.messageFont},
                                   {"ui_font_weight", settings.uiFontWeight},
                                   {"message_font_weight", settings.messageFontWeight},
                                   {"timestamps", settings.timestamps},
                                   {"avatars", settings.avatars},
                                   {"avatar_style", settings.avatarStyle},
                                   {"theme_source", settings.themeSource},
                                   {"nick_palette", settings.nickPalette},
                                   {"reduced_motion", settings.reducedMotion}}},
        {"previews", toml::table{{"links", previewModeName(settings.linkPreviews)},
                                 {"images", previewModeName(settings.imagePreviews)},
                                 {"media", previewModeName(settings.mediaPreviews)},
                                 {"max_height", settings.previewMaxHeight},
                                 {"media_max_mib", settings.previewMediaMaxMiB},
                                 {"allow_local", settings.previewAllowLocal},
                                 {"media_max_seconds", settings.previewMediaMaxSeconds}}},
        {"presentation", toml::table{{"message_layouts", std::move(messageLayouts)},
                                     {"pinned_buffers", array(settings.pinnedBuffers)}}},
        {"composer", toml::table{{"spellcheck", settings.spellcheck},
                                 {"spellcheck_language", settings.spellcheckLanguage},
                                 {"word_prediction", settings.wordPrediction}}},
        {"cursor",
         toml::table{
             {"shape", settings.cursorShape},
             {"effect", settings.cursorEffect},
             {"trail_duration_ms", cursorValue(settings.cursorTrailDurationMs, 40, 240, 120.f)},
             {"trail_strength", cursorValue(settings.cursorTrailStrength, .05, .35, .20f)}}},
        {"audio", toml::table{{"enabled", settings.sound},
                              {"master_volume", settings.masterVolume},
                              {"pitch_variation", settings.pitchVariation},
                              {"cues", std::move(cues)}}},
        {"presence",
         toml::table{{"mode", presenceName(settings.presence)}, {"channels", std::move(channels)}}},
        {"typing", toml::table{{"send", settings.typingSend}, {"receive", settings.typingReceive}}},
        {"physics", toml::table{{"gravity", settings.gravity},
                                {"drag", settings.drag},
                                {"shake", settings.shake}}},
        {"history", toml::table{{"retention_days", settings.retentionDays},
                                {"max_messages", settings.maxMessages}}},
        {"bindings", std::move(bindings)},
        {"nick_colors", mapping(settings.nickColors)},
        {"workspace", std::move(workspace)}};
}
void serverNanoseconds(sqlite3_context *context, int, sqlite3_value **values) {
    const auto *text = sqlite3_value_text(values[0]);
    const auto timestamp =
        text ? irc::parseTimestampParts({reinterpret_cast<const char *>(text),
                                         static_cast<size_t>(sqlite3_value_bytes(values[0]))})
             : std::nullopt;
    sqlite3_result_int(context, timestamp ? timestamp->nanoseconds : 0);
}
} // namespace

bool actualNickMention(std::string_view text, std::string_view nick, std::string_view caseMapping) {
    if (nick.empty())
        return false;
    const string foldedText = irc::casefold(text, caseMapping);
    const string foldedNick = irc::casefold(nick, caseMapping);
    const auto boundary = [](char c) {
        return !std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != '[' &&
               c != ']' && c != '{' && c != '}' && c != '|' && c != '\\' && c != '`';
    };
    for (size_t at = 0; (at = foldedText.find(foldedNick, at)) != string::npos;
         at += foldedNick.size())
        if ((at == 0 || boundary(foldedText[at - 1])) &&
            (at + foldedNick.size() == foldedText.size() ||
             boundary(foldedText[at + foldedNick.size()])))
            return true;
    return false;
}

string validateIdentityProfile(const Model &model, std::size_t index) {
    if (index >= model.identities.size())
        return "Choose an identity.";
    const auto &identity = model.identities[index];
    if (const auto error = identityFieldsError(identity); !error.empty())
        return string(error);
    for (std::size_t i = 0; i < model.identities.size(); ++i)
        if (i != index && model.identities[i].name == identity.name)
            return "Identity profile names must be unique.";
    return {};
}

string validateServerProfile(const Model &model, std::size_t index) {
    if (index >= model.servers.size())
        return "Choose a network.";
    const auto &server = model.servers[index];
    const ServerConfig *endpoint = &server;
    if (!server.sojuParent.empty()) {
        const auto parent =
            std::find_if(model.servers.begin(), model.servers.end(), [&](const auto &entry) {
                return entry.name == server.sojuParent && entry.sojuAccount;
            });
        if (parent == model.servers.end())
            return "Choose an existing soju account parent.";
        endpoint = &*parent;
        for (std::size_t i = 0; i < model.servers.size(); ++i)
            if (i != index && model.servers[i].sojuParent == server.sojuParent &&
                model.servers[i].sojuNetworkId == server.sojuNetworkId)
                return "This remote network is already associated with another local profile.";
    }
    const auto identity =
        std::find_if(model.identities.begin(), model.identities.end(),
                     [&](const auto &profile) { return profile.name == endpoint->identity; });
    if (identity != model.identities.end())
        if (auto error =
                validateIdentityProfile(model, std::size_t(identity - model.identities.begin()));
            !error.empty())
            return error;
    if (const auto error =
            serverFieldsError(server, identity == model.identities.end() ? nullptr : &*identity);
        !error.empty())
        return string(error);
    for (std::size_t i = 0; i < model.servers.size(); ++i)
        if (i != index && model.servers[i].name == server.name)
            return "Network names must be unique.";
    return {};
}

struct Storage::Impl {
    fs::path config = xdgDirectory("XDG_CONFIG_HOME", ".config");
    fs::path data = xdgDirectory("XDG_DATA_HOME", ".local/share");
    sqlite3 *db = nullptr;
    int retentionDays = 30, maxMessages = 100000;
    bool configSafe = true;
    struct MentionScan {
        string nick, mapping;
        int64_t sequence = 0;
    };
    map<string, MentionScan> mentionScans;
    Impl() {
        privateDirectory(config);
        privateDirectory(data);
        const fs::path database = data / "state.sqlite3";
        const int fd = ::open(database.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0)
            systemFail("open database", database);
        if (::fchmod(fd, 0600) != 0) {
            const int error = errno;
            ::close(fd);
            errno = error;
            systemFail("secure database", database);
        }
        closeChecked(fd, database);
        const int result = sqlite3_open_v2(
            database.c_str(), &db,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
        if (result != SQLITE_OK) {
            const string detail = db ? sqlite3_errmsg(db) : "out of memory";
            if (db)
                sqlite3_close(db);
            db = nullptr;
            fail("open " + database.string() + ": " + detail);
        }
        try {
            databaseCheck(db, sqlite3_busy_timeout(db, 5000), "set database busy timeout");
            databaseCheck(
                db,
                sqlite3_create_function_v2(db, "server_nanoseconds", 1,
                                           SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS,
                                           nullptr, serverNanoseconds, nullptr, nullptr, nullptr),
                "register server timestamp ordering");
            execute(db, "PRAGMA foreign_keys=ON; PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; "
                        "PRAGMA secure_delete=ON;");
            Transaction migration(db);
            int64_t version = 0;
            {
                Statement query(db, "PRAGMA user_version");
                if (query.step())
                    version = query.integer(0);
            }
            if (version > 9)
                fail("database schema " + std::to_string(version) +
                     " is newer than this BlastIRC supports; schema was not modified");
            if (version < 1)
                execute(db,
                        "CREATE TABLE meta(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
                        "CREATE TABLE buffers(id TEXT PRIMARY KEY,server TEXT NOT NULL DEFAULT "
                        "'',target TEXT NOT NULL DEFAULT '',title TEXT NOT NULL DEFAULT '',kind "
                        "TEXT NOT NULL DEFAULT 'channel',draft TEXT NOT NULL DEFAULT '',topic TEXT "
                        "NOT NULL DEFAULT '',read_marker TEXT NOT NULL DEFAULT '',unread INTEGER "
                        "NOT NULL DEFAULT 0);"
                        "CREATE TABLE messages(seq INTEGER PRIMARY KEY AUTOINCREMENT,buffer TEXT "
                        "NOT NULL,id TEXT,nick TEXT NOT NULL,text TEXT NOT NULL,time TEXT NOT "
                        "NULL,reply TEXT NOT NULL,label TEXT NOT NULL,timestamp INTEGER NOT "
                        "NULL,received_at INTEGER NOT NULL,sort_time INTEGER NOT NULL,own INTEGER "
                        "NOT NULL,pending INTEGER NOT NULL,deleted INTEGER NOT NULL,mention "
                        "INTEGER NOT NULL,action INTEGER NOT NULL,presence INTEGER NOT "
                        "NULL,local_echo INTEGER NOT NULL DEFAULT 0,UNIQUE(buffer,id));"
                        "CREATE INDEX messages_buffer_order ON messages(buffer,sort_time,seq);"
                        "CREATE INDEX messages_order ON messages(sort_time,seq);"
                        "CREATE INDEX messages_label ON messages(buffer,label);"
                        "PRAGMA user_version=1;");
            if (version < 2)
                execute(db, "CREATE TABLE reactions(message INTEGER NOT NULL REFERENCES "
                            "messages(seq) ON DELETE CASCADE,emoji TEXT NOT NULL,nick TEXT NOT "
                            "NULL,ordinal INTEGER NOT NULL,PRIMARY KEY(message,emoji,nick));"
                            "PRAGMA user_version=2;");
            if (version < 3)
                execute(db, "CREATE TABLE closed_buffers(id TEXT PRIMARY KEY NOT NULL);"
                            "PRAGMA user_version=3;");
            if (version < 4)
                execute(db, "CREATE TABLE queued_sends(seq INTEGER PRIMARY KEY AUTOINCREMENT,"
                            "buffer TEXT NOT NULL,label TEXT NOT NULL,text TEXT NOT NULL,"
                            "UNIQUE(buffer,label));"
                            "PRAGMA user_version=4;");
            if (version < 5)
                execute(db, "DROP INDEX messages_buffer_order;"
                            "DROP INDEX messages_order;"
                            "CREATE INDEX messages_buffer_order ON "
                            "messages(buffer,sort_time,server_nanoseconds(time),seq);"
                            "CREATE INDEX messages_order ON "
                            "messages(sort_time,server_nanoseconds(time),seq);"
                            "CREATE INDEX IF NOT EXISTS messages_provisional ON messages(buffer) "
                            "WHERE own=1 AND local_echo=1;"
                            "PRAGMA user_version=5;");
            if (version < 6)
                execute(db, "CREATE INDEX IF NOT EXISTS messages_time ON "
                            "messages(buffer,time,sort_time,server_nanoseconds(time),seq);"
                            "PRAGMA user_version=6;");
            if (version < 7)
                execute(db, "ALTER TABLE messages ADD activity INTEGER NOT NULL DEFAULT 0;"
                            "PRAGMA user_version=7;");
            if (version < 8)
                execute(db, "ALTER TABLE messages ADD activity_target TEXT NOT NULL DEFAULT '';"
                            "PRAGMA user_version=8;");
            if (version < 9)
                execute(db, "ALTER TABLE buffers ADD muted INTEGER NOT NULL DEFAULT 0;"
                            "ALTER TABLE buffers ADD archived INTEGER NOT NULL DEFAULT 0;"
                            "PRAGMA user_version=9;");
            migration.commit();
        } catch (...) {
            sqlite3_close(db);
            db = nullptr;
            throw;
        }
    }
    ~Impl() {
        if (db)
            sqlite3_close(db);
    }
    void trim() {
        if (retentionDays > 0) {
            Statement expired(db, "DELETE FROM messages WHERE sort_time < ?");
            expired.bind(1, nowSeconds() - static_cast<int64_t>(retentionDays) * 86400);
            expired.step();
        }
        int64_t count = 0;
        {
            Statement query(db, "SELECT COUNT(*) FROM messages");
            if (query.step())
                count = query.integer(0);
        }
        if (count > maxMessages) {
            Statement excess(db, "DELETE FROM messages WHERE seq IN (SELECT seq FROM messages "
                                 "ORDER BY sort_time,server_nanoseconds(time),seq LIMIT ?)");
            excess.bind(1, count - maxMessages);
            excess.step();
        }
    }
    void writeBuffer(const Buffer &buffer) {
        if (buffer.kind == "mentions" || buffer.kind == "search")
            return;
        Statement write(
            db,
            "INSERT INTO "
            "buffers(id,server,target,title,kind,draft,topic,read_marker,unread,muted,archived) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET "
            "server=excluded.server,target=excluded.target,title=excluded.title,kind=excluded.kind,"
            "draft=excluded.draft,topic=excluded.topic,read_marker=excluded.read_marker,unread="
            "excluded.unread,muted=excluded.muted,archived=excluded.archived");
        write.bind(1, buffer.id);
        write.bind(2, buffer.server);
        write.bind(3, buffer.target);
        write.bind(4, buffer.title);
        write.bind(5, buffer.kind);
        write.bind(6, buffer.draft);
        write.bind(7, buffer.topic);
        write.bind(8, buffer.readMarker);
        write.bind(9, static_cast<int64_t>(std::max(0, buffer.unread)));
        write.bind(10, static_cast<int64_t>(buffer.muted));
        write.bind(11, static_cast<int64_t>(buffer.archived));
        write.step();
    }
    void retireSend(const string &buffer, const string &label) {
        Statement remove(db, "DELETE FROM queued_sends WHERE buffer=? AND label=?");
        remove.bind(1, buffer);
        remove.bind(2, label);
        remove.step();
    }
    bool sendCovered(const string &buffer, const string &label, size_t parts) {
        if (!parts || parts > static_cast<size_t>(maxMessages))
            return false;
        Statement query(db, "SELECT DISTINCT label FROM messages WHERE buffer=? AND own=1 AND "
                            "deleted=0 AND (label=? OR substr(label,1,length(?))=?)");
        query.bind(1, buffer);
        query.bind(2, label);
        const string prefix = label + ".part";
        query.bind(3, prefix);
        query.bind(4, prefix);
        size_t stored = 0;
        while (query.step()) {
            const string part = query.text(0);
            if (part == label) {
                ++stored;
                continue;
            }
            const char *start = part.data() + prefix.size(), *end = part.data() + part.size();
            size_t index = 0;
            const auto parsed = std::from_chars(start, end, index);
            if (start != end && *start != '0' && parsed.ec == std::errc{} && parsed.ptr == end &&
                index > 0 && index < parts)
                ++stored;
        }
        return stored == parts;
    }
    void removeSendGhosts(const string &buffer, const string &label) {
        Statement remove(db, "DELETE FROM messages WHERE buffer=?1 AND own=1 AND local_echo=1 "
                             "AND (label=?2 OR (substr(label,1,length(?3))=?3 AND "
                             "substr(label,length(?3)+1)<>'' AND "
                             "substr(label,length(?3)+1) NOT GLOB '*[^0-9]*'))");
        remove.bind(1, buffer);
        remove.bind(2, label);
        remove.bind(3, label + ".part");
        remove.step();
    }
    void closeBuffer(const string &buffer, const vector<string> &submissions = {}) {
        Statement close(db, "INSERT OR IGNORE INTO closed_buffers(id) VALUES(?)");
        close.bind(1, buffer);
        close.step();
        // Retain authoritative source context, never a closed composer's private draft.
        Statement remove(db, "UPDATE buffers SET draft='',read_marker='',unread=0 WHERE id=?");
        remove.bind(1, buffer);
        remove.step();
        for (const auto &label : submissions)
            removeSendGhosts(buffer, label);
        // Backups may already be retired while these local echoes remain unconfirmed.
        Statement ghosts(db, "DELETE FROM messages WHERE buffer=? AND own=1 AND local_echo=1");
        ghosts.bind(1, buffer);
        ghosts.step();
        Statement discard(db, "DELETE FROM queued_sends WHERE buffer=?");
        discard.bind(1, buffer);
        discard.step();
    }
    bool persist(const string &buffer, const Message &message, const string &completedSend,
                 size_t completedParts, bool confirmed, const Buffer *source,
                 int64_t *storedSequence) {
        if (buffer.empty())
            fail("cannot store a message without a buffer ID");
        if (source && source->id != buffer)
            fail("message source context does not match its buffer ID");
        Transaction transaction(db);
        if (source && (source->kind == "channel" || source->kind == "query")) {
            Statement context(
                db, "INSERT INTO buffers(id,server,target,title,kind) VALUES(?,?,?,?,?) "
                    "ON CONFLICT(id) DO UPDATE SET server=excluded.server,target=excluded.target,"
                    "title=excluded.title,kind=excluded.kind WHERE server<>excluded.server OR "
                    "target<>excluded.target OR title<>excluded.title OR kind<>excluded.kind");
            context.bind(1, source->id);
            context.bind(2, source->server);
            context.bind(3, source->target);
            context.bind(4, source->title);
            context.bind(5, source->kind);
            context.step();
        }
        int64_t sequence = 0, received = nowSeconds();
        bool deleted = message.deleted;
        // 0: server-confirmed, 1: provisional, 2: written without an echo acknowledgement.
        int64_t localEcho = message.pending && message.own ? 1 : 0;
        {
            Statement existing(
                db, "SELECT seq,deleted,received_at,local_echo,id FROM messages WHERE "
                    "buffer=? AND ((id IS NOT NULL AND id=?) OR (local_echo IN (1,2) AND "
                    "label<>'' AND label=? AND ?=1)) ORDER BY (id=?) DESC LIMIT 1");
            existing.bind(1, buffer);
            existing.bind(2, message.id);
            existing.bind(3, message.label);
            existing.bind(4, static_cast<int64_t>(message.own));
            existing.bind(5, message.id);
            if (existing.step()) {
                sequence = existing.integer(0);
                deleted = deleted || existing.integer(1) != 0;
                received = existing.integer(2);
                if (!localEcho && existing.text(4) == message.id)
                    localEcho = existing.integer(3);
            }
        }
        // IDs assigned to otherwise unidentified rows remain usable after a restart.
        if (!sequence)
            if (const auto local = localSequence(message.id)) {
                Statement existing(db, "SELECT seq,deleted,received_at,local_echo FROM messages "
                                       "WHERE buffer=? AND seq=? AND id IS NULL");
                existing.bind(1, buffer);
                existing.bind(2, *local);
                if (existing.step()) {
                    sequence = existing.integer(0);
                    deleted = deleted || existing.integer(1) != 0;
                    received = existing.integer(2);
                    if (!localEcho)
                        localEcho = existing.integer(3);
                }
            }
        if (confirmed)
            localEcho = 0;
        else if (message.own && !message.pending && message.sendStatus == SendStatus::Sent)
            localEcho = 2;
        Statement write(
            db, sequence
                    ? "UPDATE messages SET "
                      "buffer=?,id=?,nick=?,text=?,time=?,reply=?,label=?,timestamp=?,"
                      "received_at=?,sort_time=?,own=?,pending=?,deleted=?,mention=MAX(mention,?),"
                      "action=?,presence=?,local_echo=?,activity=?,activity_target=? WHERE seq=?"
                    : "INSERT INTO "
                      "messages(buffer,id,nick,text,time,reply,label,timestamp,"
                      "received_at,sort_time,own,pending,deleted,mention,action,"
                      "presence,local_echo,activity,activity_target) "
                      "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        write.bind(1, buffer);
        if (message.id.empty())
            write.null(2);
        else
            write.bind(2, message.id);
        write.bind(3, message.nick);
        write.bind(4, deleted ? string{} : message.text);
        write.bind(5, message.time);
        write.bind(6, message.reply);
        write.bind(7, message.label);
        write.bind(8, message.timestamp);
        write.bind(9, received);
        write.bind(10, message.timestamp > 0 ? message.timestamp : received);
        write.bind(11, static_cast<int64_t>(message.own));
        write.bind(12, static_cast<int64_t>(message.pending));
        write.bind(13, static_cast<int64_t>(deleted));
        write.bind(14, static_cast<int64_t>(message.mention));
        write.bind(15, static_cast<int64_t>(message.action));
        write.bind(16, static_cast<int64_t>(message.presence));
        write.bind(17, static_cast<int64_t>(localEcho));
        write.bind(18, static_cast<int64_t>(message.activity));
        write.bind(19, message.activityTarget);
        if (sequence)
            write.bind(20, sequence);
        write.step();
        if (!sequence)
            sequence = sqlite3_last_insert_rowid(db);
        if (message.own && !localEcho && !message.label.empty()) {
            Statement ghosts(db, "DELETE FROM messages WHERE buffer=? AND local_echo IN (1,2) AND "
                                 "label=? AND seq<>?");
            ghosts.bind(1, buffer);
            ghosts.bind(2, message.label);
            ghosts.bind(3, sequence);
            ghosts.step();
        }
        {
            Statement remove(db, "DELETE FROM reactions WHERE message=?");
            remove.bind(1, sequence);
            remove.step();
        }
        if (!deleted && !message.reactions.empty()) {
            Statement reaction(
                db, "INSERT OR IGNORE INTO reactions(message,emoji,nick,ordinal) VALUES(?,?,?,?)");
            for (const auto &[emoji, nicks] : message.reactions) {
                int64_t ordinal = 0;
                for (const auto &nick : nicks) {
                    reaction.bind(1, sequence);
                    reaction.bind(2, emoji);
                    reaction.bind(3, nick);
                    reaction.bind(4, ordinal++);
                    reaction.step();
                    reaction.reset();
                }
            }
        }
        trim();
        const bool retired = !completedSend.empty() &&
                             (confirmed || sendCovered(buffer, completedSend, completedParts));
        if (retired)
            retireSend(buffer, completedSend);
        transaction.commit();
        if (storedSequence)
            *storedSequence = sequence;
        if (!message.mention && !message.own && !message.presence && !deleted) {
            const auto scanned = mentionScans.find(buffer);
            if (scanned != mentionScans.end())
                scanned->second.sequence = std::min(scanned->second.sequence, sequence - 1);
        }
        return retired;
    }
};

Storage::Storage() : impl(std::make_unique<Impl>()) {}
Storage::~Storage() = default;
string Storage::configDir() const { return impl->config.string(); }
uint64_t Storage::revision() const {
    return static_cast<uint64_t>(sqlite3_total_changes64(impl->db));
}
std::optional<Buffer> Storage::sourceBuffer(const string &id) {
    Statement query(impl->db, "SELECT id,server,target,title,kind FROM buffers WHERE id=? "
                              "AND server<>'' AND target<>'' AND kind IN ('channel','query') "
                              "AND target<>'*server*'");
    query.bind(1, id);
    if (!query.step())
        return {};
    Buffer buffer;
    buffer.id = query.text(0);
    buffer.server = query.text(1);
    buffer.target = query.text(2);
    buffer.title = query.text(3);
    buffer.kind = query.text(4);
    return buffer;
}
map<string, Buffer> Storage::mentionSources(const map<string, Network> &networks) {
    map<string, Buffer> sources;
    vector<std::pair<string, Impl::MentionScan>> scans;
    Transaction transaction(impl->db);
    Statement buffers(impl->db, "SELECT id FROM buffers WHERE server<>'' AND target<>'' "
                                "AND kind IN ('channel','query') AND target<>'*server*' "
                                "ORDER BY id");
    Statement mark(impl->db, "UPDATE messages SET mention=1 WHERE seq=? AND mention=0");
    while (buffers.step()) {
        auto source = sourceBuffer(buffers.text(0));
        if (!source)
            continue;
        const auto network = networks.find(source->server);
        const bool classify = network != networks.end() && !network->second.nick.empty();
        Impl::MentionScan scan;
        if (classify) {
            const auto previous = impl->mentionScans.find(source->id);
            if (previous != impl->mentionScans.end() &&
                previous->second.nick == network->second.nick &&
                previous->second.mapping == network->second.caseMapping)
                scan = previous->second;
            scan.nick = network->second.nick;
            scan.mapping = network->second.caseMapping;
        }
        Statement rows(impl->db,
                       "SELECT seq,COALESCE(id,'local:' || seq),nick,text,time,reply,label,"
                       "timestamp,own,pending,deleted,mention,action,presence,local_echo,"
                       "activity,activity_target,sort_time FROM messages WHERE buffer=? AND own=0 "
                       "AND presence=0 AND deleted=0 AND (mention<>0 OR (?=1 AND seq>?)) "
                       "ORDER BY sort_time,server_nanoseconds(time),seq");
        rows.bind(1, source->id);
        rows.bind(2, static_cast<int64_t>(classify));
        rows.bind(3, scan.sequence);
        while (rows.step()) {
            scan.sequence = std::max(scan.sequence, rows.integer(0));
            if (rows.integer(11) == 0) {
                if (!actualNickMention(rows.text(3), network->second.nick,
                                       network->second.caseMapping))
                    continue;
                mark.bind(1, rows.integer(0));
                mark.step();
                mark.reset();
            }
            auto message = storedMessage(impl->db, rows);
            message.mention = true;
            source->messages.push_back(std::move(message));
        }
        if (classify)
            scans.emplace_back(source->id, std::move(scan));
        if (!source->messages.empty()) {
            const string id = source->id;
            sources.emplace(id, std::move(*source));
        }
    }
    transaction.commit();
    for (auto &[id, scan] : scans)
        impl->mentionScans.insert_or_assign(id, std::move(scan));
    return sources;
}
void Storage::load(Model &model) {
    Model loaded;
    loaded.settings = defaults();
    const fs::path profiles = impl->config / "config.toml",
                   preferences = impl->config / "settings.toml";
    impl->configSafe = false;
    try {
        loaded.firstRun = !fs::exists(profiles);
        if (!loaded.firstRun && ::chmod(profiles.c_str(), 0600) != 0)
            systemFail("secure configuration", profiles);
        if (fs::exists(preferences) && ::chmod(preferences.c_str(), 0600) != 0)
            systemFail("secure settings", preferences);
        if (!loaded.firstRun)
            parseProfiles(toml::parse_file(profiles.string()), loaded);
        else {
            loaded.identities.emplace_back();
            loaded.servers.emplace_back();
        }
        if (fs::exists(preferences))
            parseSettings(toml::parse_file(preferences.string()), loaded.settings);
        validate(loaded.settings);
    } catch (const toml::parse_error &error) {
        std::ostringstream detail;
        detail << error;
        fail("cannot parse configuration; existing files were preserved: " + detail.str());
    } catch (const fs::filesystem_error &error) {
        fail(error.what());
    }
    impl->configSafe = true;
    prune(loaded.settings);
    Transaction recovery(impl->db);
    {
        Statement query(
            impl->db,
            "SELECT id,server,target,title,kind,draft,topic,read_marker,unread,muted,archived FROM "
            "buffers WHERE NOT EXISTS (SELECT 1 FROM closed_buffers WHERE "
            "closed_buffers.id=buffers.id) ORDER BY id");
        while (query.step()) {
            Buffer buffer;
            buffer.id = query.text(0);
            buffer.server = query.text(1);
            buffer.target = query.text(2);
            buffer.title = query.text(3);
            buffer.kind = query.text(4);
            buffer.draft = query.text(5);
            buffer.topic = query.text(6);
            buffer.readMarker = query.text(7);
            buffer.unread = static_cast<int>(
                std::clamp<int64_t>(query.integer(8), 0, std::numeric_limits<int>::max()));
            buffer.muted = query.integer(9) != 0;
            buffer.archived = query.integer(10) != 0;
            loaded.buffers.emplace(buffer.id, std::move(buffer));
        }
    }
    // A crash before save() must not orphan durable messages or queued submissions.
    {
        Statement query(impl->db,
                        "SELECT id FROM (SELECT buffer AS id FROM messages UNION SELECT buffer "
                        "AS id FROM queued_sends) AS orphan WHERE NOT EXISTS (SELECT 1 FROM "
                        "closed_buffers WHERE closed_buffers.id=orphan.id)");
        while (query.step()) {
            const string id = query.text(0);
            if (loaded.buffers.contains(id))
                continue;
            Buffer buffer;
            buffer.id = id;
            const auto slash = id.find('/');
            if (slash != string::npos) {
                buffer.server = id.substr(0, slash);
                buffer.target = id.substr(slash + 1);
            }
            buffer.title = buffer.target;
            if (buffer.target == "*server*")
                buffer.kind = "server";
            loaded.buffers.emplace(id, std::move(buffer));
        }
    }
    {
        Statement closed(impl->db, "SELECT id FROM closed_buffers");
        while (closed.step())
            std::erase(loaded.settings.pinnedBuffers, closed.text(0));
    }
    {
        map<string, string> drafts;
        {
            Statement queued(impl->db,
                             "SELECT buffer,label,text FROM queued_sends WHERE NOT EXISTS "
                             "(SELECT 1 FROM closed_buffers WHERE closed_buffers.id="
                             "queued_sends.buffer) ORDER BY seq");
            while (queued.step()) {
                const string id = queued.text(0), label = queued.text(1), text = queued.text(2);
                auto &draft = drafts[id];
                if (!draft.empty() && !text.empty())
                    draft += '\n';
                draft += text;
                impl->removeSendGhosts(id, label);
            }
        }
        for (auto &[id, draft] : drafts) {
            auto &buffer = loaded.buffers.at(id);
            if (!draft.empty() && !buffer.draft.empty())
                draft += '\n';
            draft += buffer.draft;
            buffer.draft = std::move(draft);
            impl->writeBuffer(buffer);
        }
        // Closed buffers intentionally discard backups without being reconstructed.
        execute(impl->db, "DELETE FROM queued_sends");
    }
    recovery.commit();
    for (auto &[id, buffer] : loaded.buffers) {
        seedChatActivity(buffer);
        Statement query(impl->db,
                        "SELECT seq,COALESCE(id,'local:' || seq),nick,text,time,reply,label,"
                        "timestamp,own,(pending OR (own=1 AND local_echo=1)),deleted,mention,"
                        "action,presence,local_echo,activity,activity_target,sort_time FROM "
                        "messages WHERE seq IN "
                        "(SELECT seq FROM (SELECT seq FROM messages WHERE buffer=?1 ORDER BY "
                        "sort_time DESC,server_nanoseconds(time) DESC,seq DESC LIMIT 200) "
                        "UNION SELECT seq FROM messages WHERE buffer=?1 AND own=1 AND "
                        "local_echo=1) ORDER BY sort_time,server_nanoseconds(time),seq");
        query.bind(1, id);
        while (query.step())
            buffer.messages.push_back(storedMessage(impl->db, query));
    }
    {
        Statement query(impl->db, "SELECT value FROM meta WHERE key='active_buffer'");
        if (query.step())
            loaded.activeBuffer = query.text(0);
    }
    if (!loaded.buffers.contains(loaded.activeBuffer))
        loaded.activeBuffer = loaded.buffers.empty() ? string{} : loaded.buffers.begin()->first;
    model.identities = std::move(loaded.identities);
    model.servers = std::move(loaded.servers);
    model.settings = std::move(loaded.settings);
    model.buffers = std::move(loaded.buffers);
    model.activeBuffer = std::move(loaded.activeBuffer);
    model.firstRun = loaded.firstRun;
}
void Storage::seedChatActivity(Buffer &buffer, bool initialize) {
    if (buffer.kind != "channel" && buffer.kind != "query")
        return;
    if (initialize) {
        buffer.chatReplaySequences.clear();
        buffer.replayChatKeys.clear();
    }
    buffer.lastChatTimestamp = buffer.lastChatSequence = 0;
    buffer.lastChatTime.clear();
    buffer.lastChatId.clear();
    Statement latest(impl->db,
                     "SELECT sort_time,time,seq,COALESCE(id,'local:' || seq) FROM messages "
                     "WHERE buffer=? AND presence=0 AND deleted=0 ORDER BY sort_time DESC,"
                     "server_nanoseconds(time) DESC,seq DESC");
    latest.bind(1, buffer.id);
    while (latest.step()) {
        if (buffer.chatReplaySequences.contains(latest.integer(2)))
            continue;
        buffer.lastChatTimestamp = latest.integer(0);
        buffer.lastChatTime = latest.text(1);
        buffer.lastChatSequence = latest.integer(2);
        buffer.lastChatId = latest.text(3);
        break;
    }
}
void Storage::saveSettings(const Settings &settings) {
    if (!impl->configSafe)
        fail("save refused after a configuration load failure; fix the configuration and reload "
             "before saving (existing files were preserved)");
    validate(settings);
    atomicWrite(impl->config / "settings.toml", formatted(settingsDocument(settings)));
}
void Storage::save(const Model &model) {
    if (!impl->configSafe)
        fail("save refused after a configuration load failure; fix the configuration and reload "
             "before saving (existing files were preserved)");
    validate(model.settings);
    const string preferences = formatted(settingsDocument(model.settings));
    // Cancelling the wizard must not turn default profiles into saved connections.
    if (!model.firstRun) {
        validateProfiles(model);
        atomicWrite(impl->config / "config.toml", formatted(profileDocument(model)));
    }
    atomicWrite(impl->config / "settings.toml", preferences);
    Transaction transaction(impl->db);
    for (const auto &[id, buffer] : model.buffers) {
        if (buffer.kind == "mentions" || buffer.kind == "search")
            continue;
        impl->writeBuffer(buffer);
        Statement reopen(impl->db, "DELETE FROM closed_buffers WHERE id=?");
        reopen.bind(1, id);
        reopen.step();
    }
    // Preserve intentional closes separately from crash-orphaned message buffers.
    vector<string> removed;
    {
        Statement query(impl->db,
                        "SELECT id FROM (SELECT id FROM buffers UNION SELECT buffer AS id FROM "
                        "messages UNION SELECT buffer AS id FROM queued_sends) WHERE id NOT IN "
                        "(SELECT id FROM closed_buffers)");
        while (query.step())
            if (!model.buffers.contains(query.text(0)))
                removed.push_back(query.text(0));
    }
    for (const auto &id : removed)
        impl->closeBuffer(id);
    {
        Statement active(impl->db, "INSERT INTO meta(key,value) VALUES('active_buffer',?) ON "
                                   "CONFLICT(key) DO UPDATE SET value=excluded.value");
        active.bind(1, model.activeBuffer);
        active.step();
    }
    impl->retentionDays = model.settings.retentionDays;
    impl->maxMessages = model.settings.maxMessages;
    impl->trim();
    transaction.commit();
}
void Storage::queueSend(const Buffer &buffer, const string &label, const string &text) {
    if (buffer.id.empty() || label.empty())
        fail("cannot queue a send without a buffer ID and submission label");
    Transaction transaction(impl->db);
    impl->writeBuffer(buffer);
    Statement reopen(impl->db, "DELETE FROM closed_buffers WHERE id=?");
    reopen.bind(1, buffer.id);
    reopen.step();
    Statement write(impl->db, "INSERT INTO queued_sends(buffer,label,text) VALUES(?,?,?) "
                              "ON CONFLICT(buffer,label) DO UPDATE SET text=excluded.text");
    write.bind(1, buffer.id);
    write.bind(2, label);
    write.bind(3, text);
    write.step();
    transaction.commit();
}
void Storage::recoverSend(const Buffer &buffer, const string &label) {
    if (buffer.id.empty() || label.empty())
        fail("cannot recover a send without a buffer ID and submission label");
    Transaction transaction(impl->db);
    impl->writeBuffer(buffer);
    impl->removeSendGhosts(buffer.id, label);
    impl->retireSend(buffer.id, label);
    transaction.commit();
}
void Storage::closeBuffer(const Buffer &buffer, const vector<string> &submissions) {
    if (buffer.id.empty())
        fail("cannot close a buffer without a buffer ID");
    Transaction transaction(impl->db);
    impl->writeBuffer(buffer);
    impl->closeBuffer(buffer.id, submissions);
    transaction.commit();
}
bool Storage::append(const string &buffer, const Message &message, const string &completedSend,
                     size_t completedParts, bool confirmed, const Buffer *source,
                     int64_t *storedSequence) {
    return impl->persist(buffer, message, completedSend, completedParts, confirmed, source,
                         storedSequence);
}
bool Storage::update(const string &buffer, const Message &message, const string &completedSend,
                     size_t completedParts, bool confirmed, const Buffer *source,
                     int64_t *storedSequence) {
    return impl->persist(buffer, message, completedSend, completedParts, confirmed, source,
                         storedSequence);
}
std::optional<Message> Storage::message(const string &buffer, const string &id) {
    if (id.empty())
        return {};
    Statement query(
        impl->db,
        "SELECT seq,COALESCE(id,'local:' || "
        "seq),nick,text,time,reply,label,timestamp,own,pending,deleted,"
        "mention,action,presence,local_echo,activity,activity_target,sort_time FROM messages WHERE "
        "buffer=? AND id=?");
    query.bind(1, buffer);
    query.bind(2, id);
    if (query.step())
        return storedMessage(impl->db, query);
    if (const auto local = localSequence(id)) {
        Statement anonymous(
            impl->db, "SELECT seq,COALESCE(id,'local:' || "
                      "seq),nick,text,time,reply,label,timestamp,own,pending,deleted,mention,"
                      "action,presence,local_echo,activity,activity_target,sort_time FROM messages "
                      "WHERE seq=? AND buffer=? "
                      "AND id IS NULL");
        anonymous.bind(1, *local);
        anonymous.bind(2, buffer);
        if (anonymous.step())
            return storedMessage(impl->db, anonymous);
    }
    return {};
}
void Storage::eraseMessage(const string &buffer, const string &id) {
    if (id.empty())
        return;
    Statement remove(impl->db, "DELETE FROM messages WHERE buffer=? AND id=?");
    remove.bind(1, buffer);
    remove.bind(2, id);
    remove.step();
    if (sqlite3_changes(impl->db) == 0)
        if (const auto local = localSequence(id)) {
            Statement anonymous(impl->db,
                                "DELETE FROM messages WHERE seq=? AND buffer=? AND id IS NULL");
            anonymous.bind(1, *local);
            anonymous.bind(2, buffer);
            anonymous.step();
        }
}
void Storage::renameBuffer(const string &oldId, const string &newId) {
    if (oldId.empty() || newId.empty())
        fail("cannot rename an empty buffer ID");
    if (oldId == newId)
        return;
    Transaction transaction(impl->db);
    bool keepClosed = false;
    {
        Statement closed(impl->db, "SELECT EXISTS(SELECT 1 FROM closed_buffers WHERE id=?1) AND "
                                   "NOT EXISTS(SELECT 1 FROM buffers WHERE id=?2 AND NOT EXISTS "
                                   "(SELECT 1 FROM closed_buffers WHERE id=?2))");
        closed.bind(1, oldId);
        closed.bind(2, newId);
        if (closed.step())
            keepClosed = closed.integer(0) != 0;
    }
    using Position = std::tuple<int64_t, int64_t, int64_t>;
    auto positionFor = [&](const string &buffer, const string &marker) -> std::optional<Position> {
        if (marker.empty())
            return {};
        Statement row(impl->db, "SELECT sort_time,server_nanoseconds(time),seq FROM messages "
                                "WHERE buffer=? AND id=?");
        row.bind(1, buffer);
        row.bind(2, marker);
        if (row.step())
            return Position{row.integer(0), row.integer(1), row.integer(2)};
        if (const auto local = localSequence(marker)) {
            Statement anonymous(impl->db,
                                "SELECT sort_time,server_nanoseconds(time),seq FROM messages "
                                "WHERE seq=? AND buffer=? AND id IS NULL");
            anonymous.bind(1, *local);
            anonymous.bind(2, buffer);
            if (anonymous.step())
                return Position{anonymous.integer(0), anonymous.integer(1), anonymous.integer(2)};
        }
        if (const auto timestamp = irc::parseTimestampParts(marker))
            return Position{timestamp->seconds, timestamp->nanoseconds,
                            std::numeric_limits<int64_t>::max()};
        return {};
    };
    string marker;
    std::optional<Position> markerPosition;
    {
        Statement markers(
            impl->db, "SELECT id,read_marker FROM buffers WHERE id IN (?,?) ORDER BY (id=?) DESC");
        markers.bind(1, oldId);
        markers.bind(2, newId);
        markers.bind(3, newId);
        while (markers.step()) {
            const string candidate = markers.text(1);
            if (candidate.empty())
                continue;
            const auto position = positionFor(markers.text(0), candidate);
            if (marker.empty() || (position && (!markerPosition || *position > *markerPosition))) {
                marker = candidate;
                markerPosition = position;
            }
        }
    }
    // Keep one row per server ID, with permanent redactions and the union of reactions.
    {
        Statement merge(
            impl->db,
            "UPDATE messages AS d SET "
            "nick=COALESCE(NULLIF(d.nick,''),s.nick),"
            "text=CASE WHEN d.deleted OR s.deleted THEN '' ELSE COALESCE(NULLIF(d.text,''),s.text) "
            "END,"
            "time=COALESCE(NULLIF(d.time,''),s.time),reply=COALESCE(NULLIF(d.reply,''),s.reply),"
            "label=COALESCE(NULLIF(d.label,''),s.label),"
            "timestamp=CASE WHEN d.timestamp=0 THEN s.timestamp ELSE d.timestamp END,"
            "sort_time=CASE WHEN d.timestamp=0 AND s.timestamp<>0 THEN s.sort_time ELSE "
            "d.sort_time END,"
            "received_at=MIN(d.received_at,s.received_at),own=MAX(d.own,s.own),pending=MIN(d."
            "pending,s.pending),"
            "deleted=MAX(d.deleted,s.deleted),mention=MAX(d.mention,s.mention),"
            "action=MAX(d.action,s.action),presence=MAX(d.presence,s.presence),"
            "local_echo=CASE WHEN d.local_echo=0 OR s.local_echo=0 THEN 0 "
            "WHEN d.local_echo=2 OR s.local_echo=2 THEN 2 ELSE 1 END,"
            "activity_target=CASE WHEN d.activity=0 THEN s.activity_target ELSE d.activity_target "
            "END,"
            "activity=CASE WHEN d.activity=0 THEN s.activity ELSE d.activity END "
            "FROM messages AS s WHERE s.buffer=? AND d.buffer=? AND s.id IS NOT NULL AND "
            "d.id=s.id");
        merge.bind(1, oldId);
        merge.bind(2, newId);
        merge.step();
    }
    {
        Statement reactions(
            impl->db,
            "INSERT OR IGNORE INTO reactions(message,emoji,nick,ordinal) "
            "SELECT d.seq,r.emoji,r.nick,r.ordinal FROM reactions AS r "
            "JOIN messages AS s ON s.seq=r.message JOIN messages AS d ON d.id=s.id AND d.buffer=? "
            "WHERE s.buffer=? AND d.deleted=0");
        reactions.bind(1, newId);
        reactions.bind(2, oldId);
        reactions.step();
    }
    {
        Statement duplicates(impl->db,
                             "DELETE FROM messages WHERE buffer=? AND id IS NOT NULL AND EXISTS "
                             "(SELECT 1 FROM messages AS d WHERE d.buffer=? AND d.id=messages.id)");
        duplicates.bind(1, oldId);
        duplicates.bind(2, newId);
        duplicates.step();
        Statement move(impl->db, "UPDATE messages SET buffer=? WHERE buffer=?");
        move.bind(1, newId);
        move.bind(2, oldId);
        move.step();
        Statement redacted(impl->db, "DELETE FROM reactions WHERE message IN (SELECT seq FROM "
                                     "messages WHERE buffer=? AND deleted=1)");
        redacted.bind(1, newId);
        redacted.step();
    }
    {
        Statement move(impl->db, "UPDATE queued_sends SET buffer=? WHERE buffer=?");
        move.bind(1, newId);
        move.bind(2, oldId);
        move.step();
    }
    const auto slash = newId.find('/');
    const string server = slash == string::npos ? string{} : newId.substr(0, slash);
    const string target = slash == string::npos ? newId : newId.substr(slash + 1);
    const string kind =
        target == "*server*"
            ? "server"
            : (!target.empty() && (target[0] == '#' || target[0] == '&') ? "channel" : "query");
    {
        Statement merge(
            impl->db,
            "INSERT INTO "
            "buffers(id,server,target,title,kind,draft,topic,read_marker,unread,muted,archived) "
            "SELECT ?,?,?,CASE WHEN title='' OR title=target THEN ? ELSE title "
            "END,kind,draft,topic,read_marker,unread,muted,archived FROM buffers WHERE id=? "
            "ON CONFLICT(id) DO UPDATE SET "
            "draft=CASE WHEN buffers.draft='' THEN excluded.draft WHEN excluded.draft='' OR "
            "buffers.draft=excluded.draft THEN buffers.draft ELSE buffers.draft || char(10) || "
            "excluded.draft END,"
            "topic=COALESCE(NULLIF(buffers.topic,''),excluded.topic),unread=MIN(2147483647,buffers."
            "unread+excluded.unread),muted=(buffers.muted OR excluded.muted),"
            "archived=(buffers.archived AND excluded.archived)");
        merge.bind(1, newId);
        merge.bind(2, server);
        merge.bind(3, target);
        merge.bind(4, target);
        merge.bind(5, oldId);
        merge.step();
        Statement create(
            impl->db,
            "INSERT OR IGNORE INTO buffers(id,server,target,title,kind) VALUES(?,?,?,?,?)");
        create.bind(1, newId);
        create.bind(2, server);
        create.bind(3, target);
        create.bind(4, target);
        create.bind(5, kind);
        create.step();
        Statement name(impl->db, "UPDATE buffers SET server=?,target=?,title=CASE WHEN title='' OR "
                                 "title=target THEN ? ELSE title END,read_marker=? WHERE id=?");
        name.bind(1, server);
        name.bind(2, target);
        name.bind(3, target);
        name.bind(4, marker);
        name.bind(5, newId);
        name.step();
        Statement remove(impl->db, "DELETE FROM buffers WHERE id=?");
        remove.bind(1, oldId);
        remove.step();
        Statement reopen(impl->db, "DELETE FROM closed_buffers WHERE id IN (?,?)");
        reopen.bind(1, oldId);
        reopen.bind(2, newId);
        reopen.step();
        if (keepClosed) {
            Statement close(impl->db, "INSERT INTO closed_buffers(id) VALUES(?)");
            close.bind(1, newId);
            close.step();
        }
        Statement active(impl->db, "UPDATE meta SET value=? WHERE key='active_buffer' AND value=?");
        active.bind(1, newId);
        active.bind(2, oldId);
        active.step();
    }
    // Resolve an ID marker again after duplicate rows have been consolidated.
    if (const auto position = positionFor(newId, marker)) {
        Statement unread(impl->db,
                         "UPDATE buffers SET unread=(SELECT COUNT(*) FROM messages WHERE buffer=? "
                         "AND (sort_time,server_nanoseconds(time),seq)>(?,?,?) "
                         "AND own=0 AND presence=0) WHERE id=?");
        unread.bind(1, newId);
        unread.bind(2, std::get<0>(*position));
        unread.bind(3, std::get<1>(*position));
        unread.bind(4, std::get<2>(*position));
        unread.bind(5, newId);
        unread.step();
    } else {
        Statement unread(impl->db, "UPDATE buffers SET unread=MIN(unread,(SELECT COUNT(*) FROM "
                                   "messages WHERE buffer=? AND own=0 AND presence=0)) WHERE id=?");
        unread.bind(1, newId);
        unread.bind(2, newId);
        unread.step();
    }
    transaction.commit();
    impl->mentionScans.erase(oldId);
    impl->mentionScans.erase(newId);
}
vector<Message> Storage::history(const string &buffer, int limit, const string &before) {
    if (limit == 0 || limit < -1)
        return {};
    if (limit > 0)
        limit = std::min(limit, impl->maxMessages);
    int64_t beforeTime = std::numeric_limits<int64_t>::max(),
            beforeSequence = std::numeric_limits<int64_t>::max();
    int64_t beforeNanoseconds = std::numeric_limits<int64_t>::max();
    if (!before.empty()) {
        bool found = false;
        // Opaque stored IDs take precedence even when they look like timestamps/local IDs.
        Statement cursor(impl->db, "SELECT sort_time,server_nanoseconds(time),seq FROM messages "
                                   "WHERE buffer=? AND id=?");
        cursor.bind(1, buffer);
        cursor.bind(2, before);
        if (cursor.step()) {
            beforeTime = cursor.integer(0);
            beforeNanoseconds = cursor.integer(1);
            beforeSequence = cursor.integer(2);
            found = true;
        } else {
            // Raw times retain their stored seconds/sequence boundary, even for legacy
            // rows whose sort_time differs from the timestamp's parsed seconds.
            Statement rawTime(impl->db,
                              "SELECT sort_time,server_nanoseconds(time),seq FROM messages "
                              "WHERE buffer=? AND time=? "
                              "ORDER BY sort_time,server_nanoseconds(time),seq LIMIT 1");
            rawTime.bind(1, buffer);
            rawTime.bind(2, before);
            if (rawTime.step()) {
                beforeTime = rawTime.integer(0);
                beforeNanoseconds = rawTime.integer(1);
                beforeSequence = rawTime.integer(2);
                found = true;
            }
            // The old local-sequence/raw-time OR selected the earlier matching tuple.
            // Reject noncanonical leading zeroes, which never matched 'local:' || seq.
            if (const auto local = localSequence(before); local && before[6] != '0') {
                Statement row(impl->db,
                              "SELECT sort_time,server_nanoseconds(time),seq FROM messages "
                              "WHERE buffer=? AND seq=?");
                row.bind(1, buffer);
                row.bind(2, *local);
                if (row.step()) {
                    const auto position =
                        std::tuple{row.integer(0), row.integer(1), row.integer(2)};
                    if (!found ||
                        position < std::tie(beforeTime, beforeNanoseconds, beforeSequence)) {
                        std::tie(beforeTime, beforeNanoseconds, beforeSequence) = position;
                        found = true;
                    }
                }
            }
        }
        if (!found) {
            const auto timestamp = irc::parseTimestampParts(before);
            if (!timestamp)
                fail("history cursor '" + before + "' was not found in buffer '" + buffer + "'");
            beforeTime = timestamp->seconds;
            beforeNanoseconds = timestamp->nanoseconds;
            beforeSequence = 0;
        }
    }
    // SQLite cannot seek every component of a row-value bound containing an expression.
    // Each disjoint tuple range contributes at most limit rows to this bounded merge.
    Statement query(impl->db, "SELECT seq,COALESCE(id,'local:' || "
                              "seq),nick,text,time,reply,label,timestamp,own,pending,deleted,"
                              "mention,action,presence,local_echo,activity,activity_target,sort_"
                              "time FROM messages WHERE seq IN "
                              "(SELECT seq FROM (SELECT seq FROM messages WHERE buffer=?1 AND "
                              "sort_time=?2 AND server_nanoseconds(time)=?3 AND seq<?4 "
                              "ORDER BY seq DESC LIMIT ?5) UNION ALL "
                              "SELECT seq FROM (SELECT seq FROM messages WHERE buffer=?1 AND "
                              "sort_time=?2 AND server_nanoseconds(time)<?3 "
                              "ORDER BY server_nanoseconds(time) DESC,seq DESC LIMIT ?5) UNION ALL "
                              "SELECT seq FROM (SELECT seq FROM messages WHERE buffer=?1 AND "
                              "sort_time<?2 ORDER BY sort_time DESC,server_nanoseconds(time) DESC,"
                              "seq DESC LIMIT ?5)) ORDER BY sort_time DESC,"
                              "server_nanoseconds(time) DESC,seq DESC LIMIT ?5");
    query.bind(1, buffer);
    query.bind(2, beforeTime);
    query.bind(3, beforeNanoseconds);
    query.bind(4, beforeSequence);
    query.bind(5, static_cast<int64_t>(limit));
    vector<Message> messages;
    while (query.step())
        messages.push_back(storedMessage(impl->db, query));
    std::reverse(messages.begin(), messages.end());
    return messages;
}
void Storage::read(const string &buffer, const string &marker) {
    Statement write(
        impl->db,
        "INSERT INTO buffers(id,server,target,title,kind,read_marker) VALUES(?,?,?,?,?,?) ON "
        "CONFLICT(id) DO UPDATE SET read_marker=excluded.read_marker,unread=0");
    const auto slash = buffer.find('/');
    const string target = slash == string::npos ? string{} : buffer.substr(slash + 1);
    write.bind(1, buffer);
    write.bind(2, slash == string::npos ? string{} : buffer.substr(0, slash));
    write.bind(3, target);
    write.bind(4, target);
    write.bind(5, target == "*server*" ? string("server") : string("channel"));
    write.bind(6, marker);
    write.step();
}
void Storage::clearHistory() {
    Transaction transaction(impl->db);
    execute(impl->db, "DELETE FROM messages; UPDATE buffers SET unread=0,read_marker='';");
    transaction.commit();
    // Checkpoint deletion without blocking other connections' read transactions.
    databaseCheck(
        impl->db,
        sqlite3_wal_checkpoint_v2(impl->db, nullptr, SQLITE_CHECKPOINT_PASSIVE, nullptr, nullptr),
        "checkpoint cleared history");
}
void Storage::clearHistory(const string &buffer) {
    Transaction transaction(impl->db);
    {
        Statement remove(impl->db, "DELETE FROM messages WHERE buffer=?");
        remove.bind(1, buffer);
        remove.step();
    }
    {
        Statement reset(impl->db, "UPDATE buffers SET unread=0,read_marker='' WHERE id=?");
        reset.bind(1, buffer);
        reset.step();
    }
    transaction.commit();
}
void Storage::prune(const Settings &settings) {
    validate(settings);
    Transaction transaction(impl->db);
    impl->retentionDays = settings.retentionDays;
    impl->maxMessages = settings.maxMessages;
    impl->trim();
    transaction.commit();
}
} // namespace blast
