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
#include <map>
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

namespace fs = std::filesystem;

struct DictionaryPair {
    fs::path aff, dic;
    std::size_t source;
};

using Dictionaries = std::map<std::string, DictionaryPair, std::less<>>;

bool regularFile(const fs::path &path) {
    std::error_code error;
    return fs::is_regular_file(path, error);
}

template <class Visitor> bool visitDictionaryPairs(const fs::path &root, Visitor visit) {
    auto visitMember = [&](const fs::path &member) {
        const auto extension = member.extension();
        if (extension != ".aff" && extension != ".dic")
            return false;
        auto id = member.stem().string();
        if (!safeId(id) || id == "auto" || !regularFile(member))
            return false;
        const bool isAff = extension == ".aff";
        auto sibling = member;
        sibling.replace_extension(isAff ? ".dic" : ".aff");
        if (!regularFile(sibling))
            return false;
        return isAff ? visit(std::move(id), member, sibling)
                     : visit(std::move(id), sibling, member);
    };

    std::error_code error;
    if (!fs::is_directory(root, error))
        return visitMember(root);
    for (fs::directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
        const auto &path = it->path();
        if (path.extension() == ".aff" && visitMember(path))
            return true;
    }
    return false;
}

Dictionaries discoverDictionaries(const std::vector<std::string> &paths) {
    Dictionaries installed;
    auto scan = [&](const fs::path &root, std::size_t source) {
        visitDictionaryPairs(root, [&](std::string id, const fs::path &aff, const fs::path &dic) {
            installed.try_emplace(std::move(id), aff, dic, source);
            return false;
        });
    };
    for (std::size_t source = 0; source < paths.size(); ++source)
        if (Spellcheck::validDictionaryPath(paths[source]))
            scan(paths[source], source);
    if (!std::string_view(BLASTIRC_SPELL_DICT_DIR).empty())
        scan(BLASTIRC_SPELL_DICT_DIR, paths.size());
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

struct DictionaryEngine {
    std::unique_ptr<Hunspell> dictionary;
    std::string encoding;
};

std::optional<DictionaryEngine> loadDictionary(const std::string &language,
                                               const DictionaryPair &pair, std::string &error) {
    if (!regularFile(pair.aff) || !regularFile(pair.dic) ||
        std::ifstream(pair.aff).peek() == std::char_traits<char>::eof() ||
        std::ifstream(pair.dic).peek() == std::char_traits<char>::eof()) {
        error = "Could not read spelling dictionary: " + language;
        return std::nullopt;
    }
    try {
        auto dictionary = std::make_unique<Hunspell>(pair.aff.c_str(), pair.dic.c_str());
        std::string encoding = dictionary->get_dic_encoding();
        auto converter = g_iconv_open(encoding.c_str(), "UTF-8");
        auto reverse = g_iconv_open("UTF-8", encoding.c_str());
        const auto failed = reinterpret_cast<GIConv>(-1);
        if (converter != failed)
            g_iconv_close(converter);
        if (reverse != failed)
            g_iconv_close(reverse);
        if (converter == failed || reverse == failed) {
            error = "Spelling dictionary encoding unavailable: " + language;
            return std::nullopt;
        }
        return DictionaryEngine{std::move(dictionary), std::move(encoding)};
    } catch (...) {
        error = "Could not load spelling dictionary: " + language;
        return std::nullopt;
    }
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
    std::vector<std::string> paths;
    std::atomic<std::uint64_t> revision{0};
    std::uint64_t loadedRevision = 0;
    std::vector<DictionaryEngine> engines;
    std::string loadedLanguage;
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

    bool currentRevision(std::uint64_t checkRevision) const {
        return !stopping.load() && revision.load() == checkRevision;
    }

    bool load(const Dictionaries &installed, std::uint64_t checkRevision, bool all,
              const std::string &language, std::string &error) {
        if (!all && !safeId(language)) {
            error = "Spelling dictionary unavailable: " + language;
            return false;
        }
        {
            std::lock_guard lock(dictionaryMutex);
            if (loadedRevision == checkRevision && loadedLanguage == language)
                return true;
        }
        std::vector<DictionaryEngine> loaded;
        if (all) {
            std::vector<Dictionaries::const_iterator> order;
            order.reserve(installed.size());
            for (auto it = installed.begin(); it != installed.end(); ++it)
                order.push_back(it);
            std::sort(order.begin(), order.end(), [](auto a, auto b) {
                if (a->second.source != b->second.source)
                    return a->second.source < b->second.source;
                return a->first < b->first;
            });
            loaded.reserve(order.size());
            for (auto it : order) {
                if (!currentRevision(checkRevision))
                    return false;
                if (auto engine = loadDictionary(it->first, it->second, error))
                    loaded.push_back(std::move(*engine));
            }
            if (loaded.empty()) {
                error = "No spelling dictionaries available";
                return false;
            }
            error.clear();
        } else {
            const auto it = installed.find(language);
            if (it == installed.end()) {
                error = "Spelling dictionary unavailable: " + language;
                return false;
            }
            if (!currentRevision(checkRevision))
                return false;
            auto engine = loadDictionary(language, it->second, error);
            if (!engine)
                return false;
            loaded.push_back(std::move(*engine));
        }
        std::lock_guard lock(dictionaryMutex);
        if (!currentRevision(checkRevision))
            return false;
        engines.swap(loaded);
        loadedRevision = checkRevision;
        loadedLanguage = language;
        return true;
    }

    SpellResult check(const Request &request, const Dictionaries &installed,
                      std::uint64_t checkRevision) {
        SpellResult checked{request.ticket, resolveLanguage(request.language), {}, {}};
        const bool all = request.language == Spellcheck::allLanguages;
        if (!load(installed, checkRevision, all, checked.language, checked.error) ||
            !current(request))
            return checked;
        if (request.text.size() >= static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
            !g_utf8_validate(request.text.data(), request.text.size(), nullptr)) {
            checked.error = "Spelling requires valid UTF-8 text within Pango's size limit";
            return checked;
        }
        const auto characters = g_utf8_strlen(request.text.data(), request.text.size());
        std::vector<PangoLogAttr> attributes(static_cast<std::size_t>(characters) + 1);
        auto *language = all ? pango_language_get_default()
                             : pango_language_from_string(checked.language.c_str());
        pango_get_log_attrs(request.text.data(), static_cast<int>(request.text.size()), -1,
                            language, attributes.data(), static_cast<int>(attributes.size()));
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
                    bool accepted = false;
                    for (auto &engine : engines) {
                        auto encoded = convert(word, engine.encoding.c_str(), "UTF-8");
                        if (!encoded && !all) {
                            checked.error =
                                "Text cannot be represented in spelling dictionary encoding: " +
                                checked.language;
                            checked.misspelled.clear();
                            return checked;
                        }
                        if (encoded && engine.dictionary->spell(*encoded)) {
                            accepted = true;
                            break;
                        }
                    }
                    if (!accepted)
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
        Dictionaries installed;
        std::optional<std::uint64_t> scannedRevision;
        while (!stopping.load()) {
            std::unique_lock lock(mutex);
            wake.wait(lock, [&] { return stopping.load() || pending.has_value(); });
            if (stopping.load())
                return;
            auto request = std::move(*pending);
            pending.reset();
            const auto checkRevision = revision.load();
            if (scannedRevision != checkRevision) {
                auto scanPaths = paths;
                lock.unlock();
                installed = discoverDictionaries(scanPaths);
                scannedRevision = checkRevision;
            } else {
                lock.unlock();
            }
            auto checked = check(request, installed, checkRevision);
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
    const auto resolved = resolveLanguage(language);
    if (language != allLanguages && !safeId(resolved))
        return suggestions;
    std::lock_guard lock(state_->dictionaryMutex);
    if (state_->loadedRevision != state_->revision.load() || state_->loadedLanguage != resolved)
        return suggestions;
    for (auto &engine : state_->engines) {
        const auto encoded = convert(word, engine.encoding.c_str(), "UTF-8");
        if (!encoded)
            continue;
        for (const auto &candidate : engine.dictionary->suggest(*encoded)) {
            auto decoded = convert(candidate, "UTF-8", engine.encoding.c_str());
            if (decoded && decoded->size() <= 256 &&
                g_utf8_validate(decoded->data(), decoded->size(), nullptr) &&
                std::find(suggestions.begin(), suggestions.end(), *decoded) == suggestions.end())
                suggestions.push_back(std::move(*decoded));
            if (suggestions.size() == 8)
                return suggestions;
        }
    }
    return suggestions;
}

void Spellcheck::setDictionaryPaths(const std::vector<std::string> &paths) {
    std::scoped_lock lock(state_->mutex, state_->dictionaryMutex);
    state_->paths = paths;
    state_->revision.fetch_add(1);
    state_->generation.fetch_add(1);
    state_->pending.reset();
    state_->result.reset();
}

std::vector<std::string> Spellcheck::languages(const std::vector<std::string> &paths) {
    const auto installed = discoverDictionaries(paths);
    std::vector<std::string> ids;
    ids.reserve(installed.size());
    for (const auto &entry : installed)
        ids.push_back(entry.first);
    return ids;
}

bool Spellcheck::hasDictionaryPair(std::string_view path) {
    return validDictionaryPath(path) &&
           visitDictionaryPairs(fs::path(path),
                                [](const auto &, const auto &, const auto &) { return true; });
}

} // namespace blast
