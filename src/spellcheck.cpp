#include "spellcheck.hpp"

#include <hunspell/hunspell.hxx>
#include <pango/pango.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

#ifndef BLASTIRC_SPELL_DICT_DIR
#define BLASTIRC_SPELL_DICT_DIR ""
#endif

namespace blast {
namespace {

bool asciiAlnum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool safeId(std::string_view id) {
    return !id.empty() && asciiAlnum(id.front()) && std::all_of(id.begin(), id.end(), [](char c) {
        return asciiAlnum(c) || c == '_' || c == '-';
    });
}

std::string resolveLanguage(std::string_view requested) {
    if (requested != "auto")
        return std::string(requested);
    const char *locale = std::getenv("LANG");
    std::string id = locale ? locale : "";
    const auto suffix = id.find_first_of(".@");
    if (suffix != std::string::npos)
        id.resize(suffix);
    if (id.empty() || id == "C" || id == "POSIX")
        return "en_US";
    if (!safeId(id))
        return id;
    std::replace(id.begin(), id.end(), '-', '_');
    const auto separator = id.find('_');
    for (std::size_t i = 0; i < id.size(); ++i) {
        if (i < separator)
            id[i] = g_ascii_tolower(id[i]);
        else if (i > separator)
            id[i] = g_ascii_toupper(id[i]);
    }
    return id;
}

std::filesystem::path dictionaryPath(std::string_view id, std::string_view extension) {
    return std::filesystem::path(BLASTIRC_SPELL_DICT_DIR) /
           (std::string(id) + std::string(extension));
}

const std::vector<std::string> &installedLanguages() {
    static const auto installed = [] {
        std::vector<std::string> ids;
        if (std::string_view(BLASTIRC_SPELL_DICT_DIR).empty())
            return ids;
        std::error_code error;
        for (std::filesystem::directory_iterator it(BLASTIRC_SPELL_DICT_DIR, error), end;
             !error && it != end; it.increment(error)) {
            const auto &path = it->path();
            if (path.extension() != ".aff")
                continue;
            auto id = path.stem().string();
            if (!safeId(id))
                continue;
            if (std::filesystem::is_regular_file(path, error) && !error &&
                std::filesystem::is_regular_file(dictionaryPath(id, ".dic"), error) && !error)
                ids.push_back(std::move(id));
            error.clear();
        }
        std::sort(ids.begin(), ids.end());
        return ids;
    }();
    return installed;
}

std::optional<std::string> convert(std::string_view text, const char *to, const char *from) {
    if (g_ascii_strcasecmp(to, from) == 0)
        return std::string(text);
    GError *error = nullptr;
    gsize written = 0;
    char *bytes = g_convert(text.data(), static_cast<gssize>(text.size()), to, from, nullptr,
                            &written, &error);
    if (!bytes) {
        g_clear_error(&error);
        return std::nullopt;
    }
    std::string converted(bytes, written);
    g_free(bytes);
    return converted;
}

} // namespace

struct Spellcheck::State {
    struct Request {
        std::uint64_t generation, ticket;
        std::string text, language;
        std::vector<SpellRange> excluded;
    };

    std::mutex mutex, dictionaryMutex;
    std::condition_variable wake;
    std::optional<Request> pending;
    std::optional<SpellResult> result;
    std::atomic<std::uint64_t> generation{0};
    std::atomic<bool> stopping{false};
    std::unique_ptr<Hunspell> dictionary;
    std::string dictionaryLanguage, encoding;
    std::thread worker;

    State() : worker([this] { run(); }) {}

    ~State() {
        {
            std::lock_guard lock(mutex);
            stopping.store(true);
        }
        wake.notify_one();
        worker.join();
    }

    bool current(const Request &request) const {
        return !stopping.load() && generation.load() == request.generation;
    }

    bool load(const std::string &language, std::string &error) {
        // Loading stays off the dictionary lock: explicit suggestions wait at most for one checked
        // word.
        {
            std::lock_guard lock(dictionaryMutex);
            if (dictionary && dictionaryLanguage == language)
                return true;
        }
        const auto &ids = installedLanguages();
        if (!safeId(language) || std::find(ids.begin(), ids.end(), language) == ids.end()) {
            error = "Spelling dictionary unavailable: " + language;
            return false;
        }
        std::ifstream aff(dictionaryPath(language, ".aff"));
        std::ifstream dic(dictionaryPath(language, ".dic"));
        if (aff.peek() == std::char_traits<char>::eof() ||
            dic.peek() == std::char_traits<char>::eof()) {
            error = "Could not read spelling dictionary: " + language;
            return false;
        }
        try {
            auto loaded = std::make_unique<Hunspell>(dictionaryPath(language, ".aff").c_str(),
                                                     dictionaryPath(language, ".dic").c_str());
            std::string loadedEncoding = loaded->get_dic_encoding();
            auto converter = g_iconv_open(loadedEncoding.c_str(), "UTF-8");
            auto reverse = g_iconv_open("UTF-8", loadedEncoding.c_str());
            const bool valid = converter != reinterpret_cast<GIConv>(-1) &&
                               reverse != reinterpret_cast<GIConv>(-1);
            if (converter != reinterpret_cast<GIConv>(-1))
                g_iconv_close(converter);
            if (reverse != reinterpret_cast<GIConv>(-1))
                g_iconv_close(reverse);
            if (!valid) {
                error = "Spelling dictionary encoding unavailable: " + language;
                return false;
            }
            std::lock_guard lock(dictionaryMutex);
            dictionary.swap(loaded);
            encoding = std::move(loadedEncoding);
            dictionaryLanguage = language;
            return true;
        } catch (...) {
            error = "Could not load spelling dictionary: " + language;
            return false;
        }
    }

    SpellResult check(const Request &request) {
        SpellResult checked{request.ticket, resolveLanguage(request.language), {}, {}};
        if (!load(checked.language, checked.error))
            return checked;
        if (request.text.size() >= static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
            !g_utf8_validate(request.text.data(), request.text.size(), nullptr)) {
            checked.error = "Spelling requires valid UTF-8 text within Pango's size limit";
            return checked;
        }
        const auto characters = g_utf8_strlen(request.text.data(), request.text.size());
        std::vector<PangoLogAttr> attributes(static_cast<std::size_t>(characters) + 1);
        pango_get_log_attrs(request.text.data(), static_cast<int>(request.text.size()), -1,
                            pango_language_from_string(checked.language.c_str()), attributes.data(),
                            static_cast<int>(attributes.size()));
        const char *origin = request.text.data();
        const char *cursor = origin;
        std::optional<std::size_t> begin;
        bool hasLetter = false;
        for (std::size_t i = 0; i < attributes.size(); ++i) {
            if (!current(request))
                return checked;
            const auto offset = static_cast<std::size_t>(cursor - origin);
            if (attributes[i].is_word_end && begin) {
                const SpellRange range{*begin, offset};
                const bool excluded = std::any_of(
                    request.excluded.begin(), request.excluded.end(), [&](const SpellRange &skip) {
                        return skip.begin < range.end && skip.end > range.begin;
                    });
                if (hasLetter && !excluded) {
                    const std::string_view word(origin + range.begin, range.end - range.begin);
                    std::lock_guard lock(dictionaryMutex);
                    auto encoded = convert(word, encoding.c_str(), "UTF-8");
                    if (!encoded) {
                        checked.error =
                            "Text cannot be represented in spelling dictionary encoding: " +
                            checked.language;
                        checked.misspelled.clear();
                        return checked;
                    }
                    if (!dictionary->spell(*encoded))
                        checked.misspelled.push_back(range);
                }
                begin.reset();
            }
            if (attributes[i].is_word_start) {
                begin = offset;
                hasLetter = false;
            }
            if (i + 1 < attributes.size()) {
                hasLetter = hasLetter || g_unichar_isalpha(g_utf8_get_char(cursor));
                cursor = g_utf8_next_char(cursor);
            }
        }
        return checked;
    }

    void run() {
        while (!stopping.load()) {
            std::unique_lock lock(mutex);
            wake.wait(lock, [&] { return stopping.load() || pending.has_value(); });
            if (stopping.load())
                return;
            auto request = std::move(*pending);
            pending.reset();
            lock.unlock();
            auto checked = check(request);
            lock.lock();
            if (current(request))
                result = std::move(checked);
        }
    }
};

Spellcheck::Spellcheck() : state_(std::make_unique<State>()) {}
Spellcheck::~Spellcheck() = default;

void Spellcheck::request(std::uint64_t ticket, std::string text, std::string language,
                         std::vector<SpellRange> excluded) {
    std::lock_guard lock(state_->mutex);
    const auto generation = state_->generation.fetch_add(1) + 1;
    state_->pending = State::Request{generation, ticket, std::move(text), std::move(language),
                                     std::move(excluded)};
    state_->result.reset();
    state_->wake.notify_one();
}

std::optional<SpellResult> Spellcheck::poll() {
    std::lock_guard lock(state_->mutex);
    return std::exchange(state_->result, std::nullopt);
}

std::vector<std::string> Spellcheck::suggest(std::string_view word, std::string_view language) {
    std::vector<std::string> suggestions;
    if (word.empty() || word.size() > 128 || word.find('\0') != std::string_view::npos ||
        !g_utf8_validate(word.data(), word.size(), nullptr))
        return suggestions;
    for (const char *p = word.data(); p < word.data() + word.size(); p = g_utf8_next_char(p)) {
        const auto c = g_utf8_get_char(p);
        if (!g_unichar_isalpha(c) && !g_unichar_ismark(c) && c != '\'' && c != 0x2019 && c != '-')
            return suggestions;
    }
    std::lock_guard lock(state_->dictionaryMutex);
    if (!state_->dictionary || state_->dictionaryLanguage != resolveLanguage(language))
        return suggestions;
    const auto encoded = convert(word, state_->encoding.c_str(), "UTF-8");
    if (!encoded)
        return suggestions;
    for (const auto &candidate : state_->dictionary->suggest(*encoded)) {
        auto decoded = convert(candidate, "UTF-8", state_->encoding.c_str());
        if (decoded && decoded->size() <= 256 &&
            g_utf8_validate(decoded->data(), decoded->size(), nullptr))
            suggestions.push_back(std::move(*decoded));
        if (suggestions.size() == 8)
            break;
    }
    return suggestions;
}

std::vector<std::string> Spellcheck::languages() { return installedLanguages(); }

} // namespace blast
