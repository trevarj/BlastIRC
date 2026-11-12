#include "word_completion.hpp"

#include <glib.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <fstream>
#include <map>
#include <thread>
#include <utility>
#include <vector>

#ifndef BLASTIRC_WORDLIST_FILE
#define BLASTIRC_WORDLIST_FILE ""
#endif

namespace blast {
namespace {
constexpr std::size_t maxWordBytes = 128;
constexpr std::size_t maxFileBytes = 16 * 1024 * 1024;
constexpr std::size_t maxSnippetBytes = 512;
constexpr std::size_t maxSnippets = 32;
using Counts = std::map<std::string, double, std::less<>>;
using Contexts = std::map<std::string, Counts, std::less<>>;

bool validUtf8(std::string_view text) {
    if (text.empty())
        return true;
    return text.find('\0') == text.npos &&
           g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr);
}

bool letter(gunichar ch) { return g_unichar_isalpha(ch) || g_unichar_ismark(ch); }
bool joiner(gunichar ch) { return ch == '\'' || ch == '-' || ch == 0x2019; }

bool validWord(std::string_view word) {
    if (word.empty() || word.size() > maxWordBytes || !validUtf8(word))
        return false;
    bool previousLetter = false;
    for (const char *p = word.data(), *end = p + word.size(); p < end; p = g_utf8_next_char(p)) {
        const auto ch = g_utf8_get_char(p);
        if (letter(ch))
            previousLetter = true;
        else if (joiner(ch) && previousLetter)
            previousLetter = false;
        else
            return false;
    }
    return previousLetter;
}

std::string lower(std::string_view text) {
    auto *folded = g_utf8_strdown(text.data(), static_cast<gssize>(text.size()));
    std::string result(folded);
    g_free(folded);
    return result;
}

std::string boundedSnippet(std::string_view text) {
    // Do not turn a truncated UTF-8 codepoint into a malformed history entry.
    if (text.size() > maxSnippetBytes) {
        text = text.substr(0, maxSnippetBytes);
        auto end = text.size();
        while (end && (static_cast<unsigned char>(text[end - 1]) & 0xc0) == 0x80)
            --end;
        if (end && static_cast<unsigned char>(text[end - 1]) >= 0xc0) {
            const auto lead = static_cast<unsigned char>(text[end - 1]);
            const std::size_t length = lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
            if (lead >= 0xc2 && lead <= 0xf4 && text.size() - (end - 1) < length)
                text = text.substr(0, end - 1);
        }
    }
    return validUtf8(text) ? std::string(text) : std::string{};
}

template <typename Word, typename Boundary>
void tokenize(std::string_view text, Word word, Boundary boundary) {
    if (text.empty())
        return;
    const char *start = nullptr;
    const char *end = text.data() + text.size();
    const auto finish = [&](const char *p) {
        if (start) {
            std::string_view token(start, p - start);
            if (validWord(token))
                word(lower(token));
            else
                boundary();
            start = nullptr;
        }
    };
    for (const char *p = text.data(); p < end; p = g_utf8_next_char(p)) {
        const auto ch = g_utf8_get_char(p);
        const auto *next = g_utf8_next_char(p);
        if (letter(ch) || (start && joiner(ch) && next < end && letter(g_utf8_get_char(next)))) {
            if (!start)
                start = p;
        } else {
            finish(p);
            if (ch != ' ' && ch != '\t')
                boundary();
        }
    }
    finish(end);
}

std::string contextKey(std::string_view previous2, std::string_view previous1) {
    std::string key(previous2);
    key += '\t';
    key += previous1;
    return key;
}
} // namespace

struct WordCompletion::State {
    std::atomic<bool> loaded{false};
    Counts prior;
    std::vector<std::string> history;
    Counts local;
    Contexts bigrams;
    Contexts trigrams;
    double localTotal = 0;
    std::jthread worker;

    explicit State(std::string path)
        : worker([this, path = std::move(path)] {
              try {
                  std::ifstream input(path, std::ios::binary);
                  std::string line;
                  line.reserve(maxWordBytes + 8);
                  bool oversized = false;
                  std::size_t bytes = 0;
                  const auto append = [&] {
                      const auto tab = line.find('\t');
                      if (!oversized && tab != line.npos &&
                          validWord(std::string_view(line).substr(0, tab))) {
                          const auto binText = std::string_view(line).substr(tab + 1);
                          unsigned bin = 0;
                          const auto parsed =
                              std::from_chars(binText.data(), binText.data() + binText.size(), bin);
                          if (!binText.empty() && parsed.ec == std::errc{} &&
                              parsed.ptr == binText.data() + binText.size() && bin <= 1000) {
                              const auto probability =
                                  std::pow(10.0, -static_cast<double>(bin) / 100.0);
                              auto &stored = prior[lower(std::string_view(line).substr(0, tab))];
                              stored = std::max(stored, probability);
                          }
                      }
                      line.clear();
                      oversized = false;
                  };
                  char byte;
                  while (input.get(byte)) {
                      if (++bytes > maxFileBytes) {
                          prior.clear();
                          break;
                      }
                      if (byte == '\n')
                          append();
                      else if (line.size() < maxWordBytes + 8)
                          line.push_back(byte);
                      else
                          oversized = true;
                  }
                  if (bytes <= maxFileBytes) {
                      if (input.bad())
                          prior.clear();
                      else
                          append();
                  }
              } catch (...) {
                  prior.clear();
              }
              loaded.store(true, std::memory_order_release);
          }) {}
};

WordCompletion::WordCompletion(std::string path)
    : state_(std::make_unique<State>(path.empty() ? BLASTIRC_WORDLIST_FILE : std::move(path))) {}

WordCompletion::~WordCompletion() = default;

bool WordCompletion::ready() const { return state_->loaded.load(std::memory_order_acquire); }

void WordCompletion::setHistory(std::span<const std::string> newestFirst) {
    std::vector<std::string> history;
    history.reserve(std::min(newestFirst.size(), maxSnippets));
    for (const auto &text : newestFirst.first(std::min(newestFirst.size(), maxSnippets)))
        history.push_back(boundedSnippet(text));
    if (history == state_->history)
        return;
    state_->history = std::move(history);
    state_->local.clear();
    state_->bigrams.clear();
    state_->trigrams.clear();
    state_->localTotal = 0;
    for (std::size_t i = 0; i < state_->history.size(); ++i) {
        const double weight = 1.0 / (1.0 + static_cast<double>(i) / 8.0);
        std::string previous1, previous2;
        tokenize(
            state_->history[i],
            [&](std::string word) {
                state_->local[word] += weight;
                state_->localTotal += weight;
                if (!previous1.empty())
                    state_->bigrams[previous1][word] += weight;
                if (!previous2.empty())
                    state_->trigrams[contextKey(previous2, previous1)][word] += weight;
                previous2 = std::move(previous1);
                previous1 = std::move(word);
            },
            [&] {
                previous1.clear();
                previous2.clear();
            });
    }
}

std::string WordCompletion::complete(std::string_view before, std::string_view prefix) {
    if (!validWord(prefix) || !validUtf8(before))
        return {};
    if (before.size() > maxSnippetBytes) {
        before.remove_prefix(before.size() - maxSnippetBytes);
        while (!before.empty() && (static_cast<unsigned char>(before.front()) & 0xc0) == 0x80)
            before.remove_prefix(1);
        // A clipped first word is not context.
        const auto space = before.find_first_of(" \t\n\r");
        before = space == before.npos ? std::string_view{} : before.substr(space + 1);
    }
    std::string previous1, previous2;
    tokenize(
        before,
        [&](std::string word) {
            previous2 = std::move(previous1);
            previous1 = std::move(word);
        },
        [&] {
            previous1.clear();
            previous2.clear();
        });
    const auto folded = lower(prefix);
    const auto context = [&](const Contexts &contexts, const std::string &key) -> const Counts * {
        const auto found = contexts.find(key);
        return found == contexts.end() ? nullptr : &found->second;
    };
    const auto *bigram = context(state_->bigrams, previous1);
    const auto *trigram = context(state_->trigrams, contextKey(previous2, previous1));
    const auto scale = [](const Counts *counts, double weight) {
        double total = 0;
        if (counts)
            for (const auto &[word, count] : *counts)
                total += count;
        return total ? weight / total : 0;
    };
    const double bigramScale = scale(bigram, 0.65);
    const double trigramScale = scale(trigram, 1.5);
    const double localScale = state_->localTotal ? 0.2 / state_->localTotal : 0;
    const auto count = [](const Counts *counts, std::string_view word) {
        if (counts) {
            const auto found = counts->find(word);
            if (found != counts->end())
                return found->second;
        }
        return 0.0;
    };
    static const Counts empty;
    const auto &prior = ready() ? state_->prior : empty;
    auto general = prior.lower_bound(folded);
    auto local = state_->local.lower_bound(folded);
    std::string_view best;
    double score = 0;
    while ((general != prior.end() && general->first.starts_with(folded)) ||
           (local != state_->local.end() && local->first.starts_with(folded))) {
        const bool useGeneral = general != prior.end() && general->first.starts_with(folded) &&
                                (local == state_->local.end() || general->first <= local->first);
        const std::string_view word = useGeneral ? general->first : local->first;
        const double value = count(&prior, word) + count(&state_->local, word) * localScale +
                             count(bigram, word) * bigramScale +
                             count(trigram, word) * trigramScale;
        if (value > score) {
            score = value;
            best = word;
        }
        if (general != prior.end() && general->first == word)
            ++general;
        if (local != state_->local.end() && local->first == word)
            ++local;
    }
    if (best.empty() || best == folded)
        return {};
    bool uppercase = true;
    bool cased = false;
    for (const char *p = prefix.data(), *end = p + prefix.size(); p < end;
         p = g_utf8_next_char(p)) {
        const auto ch = g_utf8_get_char(p);
        if (g_unichar_islower(ch))
            uppercase = false;
        cased = cased || g_unichar_isupper(ch) || g_unichar_islower(ch);
    }
    const auto suffix = best.substr(folded.size());
    std::string result(prefix);
    if (uppercase && cased) {
        auto *upper = g_utf8_strup(suffix.data(), static_cast<gssize>(suffix.size()));
        result += upper;
        g_free(upper);
    } else {
        result += suffix;
    }
    return result;
}

} // namespace blast
