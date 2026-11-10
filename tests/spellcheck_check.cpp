#include "spellcheck.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

using namespace std::chrono_literals;

namespace {
blast::SpellResult await(blast::Spellcheck &checker, std::uint64_t ticket) {
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto result = checker.poll()) {
            assert(result->ticket == ticket);
            return std::move(*result);
        }
        std::this_thread::sleep_for(1ms);
    }
    assert(false && "spellcheck worker did not finish");
    std::abort();
}

bool contains(const std::vector<std::string> &words, const std::string &word) {
    return std::find(words.begin(), words.end(), word) != words.end();
}
} // namespace

int main() {
    const auto languages = blast::Spellcheck::languages();
    assert(contains(languages, "en_US") && "checks require the real packaged en_US dictionary");
    for (const auto &language : languages) {
        assert(!language.empty());
        assert(language.find_first_not_of(
                   "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") ==
               std::string::npos);
    }

    blast::Spellcheck checker;
    checker.request(1, "hello world wrold", "en_US");
    auto result = await(checker, 1);
    assert(result.error.empty() && result.language == "en_US");
    assert(result.misspelled.size() == 1);
    assert(result.misspelled[0].begin == 12 && result.misspelled[0].end == 17);
    const auto suggestions = checker.suggest("wrold", "en_US");
    assert(contains(suggestions, "world")); // No dependency on incidental ranking.
    assert(suggestions.size() <= 8);
    assert(checker.suggest(std::string(129, 'x'), "en_US").empty());
    assert(checker.suggest("two words", "en_US").empty());
    assert(checker.suggest("wrold", "zz_ZZ").empty());

    const std::string unicode = "hello éézzzzzz world";
    checker.request(2, unicode, "en_US");
    result = await(checker, 2);
    assert(result.error.empty() && result.misspelled.size() == 1);
    assert(result.misspelled[0].begin == 6);
    assert(unicode.substr(result.misspelled[0].begin,
                          result.misspelled[0].end - result.misspelled[0].begin) == "éézzzzzz");

    const std::string exclusions =
        "wrold https://zzzzzz.invalid #zzzzzz @zzzzzz 12345 １２３４５ zzzzzzz";
    const auto last = exclusions.rfind(' ') + 1;
    checker.request(3, exclusions, "en_US", {{0, 5}, {6, last}});
    result = await(checker, 3);
    assert(result.error.empty() && result.misspelled.size() == 1);
    assert(result.misspelled.front().begin == last &&
           result.misspelled.front().end == exclusions.size());
    checker.request(4, "12345 １２３４５", "en_US");
    result = await(checker, 4);
    assert(result.error.empty() && result.misspelled.empty());

    // The tail must be checked too, not an arbitrary prefix presented as complete.
    std::string longDraft;
    for (int i = 0; i < 20000; ++i)
        longDraft += "hello ";
    const auto tail = longDraft.size();
    longDraft += "wrold";
    checker.request(5, longDraft, "en_US");
    result = await(checker, 5);
    assert(result.error.empty() && result.misspelled.size() == 1);
    assert(result.misspelled.front().begin == tail);

    // Already-published stale results are discarded, as are in-flight snapshots.
    checker.request(6, "wrold", "en_US");
    std::this_thread::sleep_for(20ms);
    checker.request(7, longDraft, "en_US");
    for (int i = 0; i < 10; ++i)
        assert(contains(checker.suggest("wrold", "en_US"), "world"));
    checker.request(8, "hello", "en_US");
    result = await(checker, 8);
    assert(result.error.empty() && result.misspelled.empty());
    assert(!checker.poll());
    checker.request(8, longDraft, "en_US"); // Reused tickets still use request order.
    checker.request(2, "world", "en_US");
    result = await(checker, 2);
    assert(result.error.empty() && result.misspelled.empty());

    checker.request(9, "hello", "zz_ZZ");
    result = await(checker, 9);
    assert(!result.error.empty() && result.language == "zz_ZZ" && result.misspelled.empty());
    checker.request(10, "hello", "../../en_US");
    assert(!await(checker, 10).error.empty());
    checker.request(11, std::string("hello \xff", 7), "en_US");
    assert(!await(checker, 11).error.empty());

    const char *old = std::getenv("LANG");
    const bool hadLocale = old != nullptr;
    const std::string savedLocale = old ? old : "";
    setenv("LANG", "C.UTF-8", 1);
    checker.request(12, "hello", "auto");
    result = await(checker, 12);
    assert(result.language == "en_US" && result.error.empty());
    setenv("LANG", "en-us.UTF-8@modifier", 1);
    checker.request(13, "hello", "auto");
    result = await(checker, 13);
    assert(result.language == "en_US" && result.error.empty());
    setenv("LANG", "zz_ZZ.UTF-8", 1);
    checker.request(14, "hello", "auto");
    result = await(checker, 14);
    assert(result.language == "zz_ZZ" && !result.error.empty());
    setenv("LANG", "POSIX", 1);
    checker.request(15, "hello", "auto");
    result = await(checker, 15);
    assert(result.language == "en_US" && result.error.empty());
    unsetenv("LANG");
    checker.request(16, "hello", "auto");
    result = await(checker, 16);
    assert(result.language == "en_US" && result.error.empty());
    if (hadLocale)
        setenv("LANG", savedLocale.c_str(), 1);
    else
        unsetenv("LANG");
    for (int i = 0; i < 32; ++i) {
        blast::Spellcheck destroyedWhileIdle;
    }

    {
        blast::Spellcheck destroyedWhileBusy;
        destroyedWhileBusy.request(1, longDraft, "en_US");
        destroyedWhileBusy.request(2, "wrold", "en_US");
    } // Joining with in-flight and pending work must be safe.
}
