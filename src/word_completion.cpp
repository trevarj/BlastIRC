#include "word_completion.hpp"

#include <glib.h>

#include <algorithm>
#include <array>
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
constexpr std::size_t maxPredictedWords = 3;
using Counts = std::map<std::string, double, std::less<>>;
using Contexts = std::map<std::string, Counts, std::less<>>;

struct RankedWord {
    std::string_view word;
    double score;
};

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

std::string_view boundedSnippet(std::string_view text) {
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
    return validUtf8(text) ? text : std::string_view{};
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

const Counts *findContext(const Contexts &contexts, std::string_view key) {
    const auto found = contexts.find(key);
    return found == contexts.end() ? nullptr : &found->second;
}
} // namespace

struct WordCompletion::State {
    std::atomic<bool> loaded{false};
    Counts prior;
    std::vector<std::string> history;
    Counts local;
    Contexts bigrams;
    std::map<std::string, Contexts, std::less<>> trigrams;
    double localTotal = 0;
    std::jthread worker;

    std::size_t complete(std::string_view before, std::string_view prefix,
                         std::span<RankedWord> ranks, std::span<std::string> output,
                         bool extensionsOnly);

    const Counts *trigram(std::string_view previous2, std::string_view previous1) const {
        const auto found = trigrams.find(previous2);
        return found == trigrams.end() ? nullptr : findContext(found->second, previous1);
    }

    void extend(std::string_view previous2, std::string_view previous1, std::size_t remaining,
                std::string &result) const {
        for (; remaining && !previous1.empty(); --remaining) {
            const auto *next =
                previous2.empty() ? findContext(bigrams, previous1) : trigram(previous2, previous1);
            if (!next || next->size() != 1 || next->begin()->first.empty())
                break;
            if (!result.empty())
                result += ' ';
            previous2 = previous1;
            previous1 = next->begin()->first;
            result += previous1;
        }
    }

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
    newestFirst = newestFirst.first(std::min(newestFirst.size(), maxSnippets));
    if (std::ranges::equal(newestFirst, state_->history, {}, boundedSnippet))
        return;
    state_->history.clear();
    state_->history.reserve(newestFirst.size());
    for (const auto &text : newestFirst)
        state_->history.emplace_back(boundedSnippet(text));
    state_->local.clear();
    state_->bigrams.clear();
    state_->trigrams.clear();
    state_->localTotal = 0;
    for (std::size_t i = 0; i < state_->history.size(); ++i) {
        const double weight = 1.0 / (1.0 + static_cast<double>(i) / 8.0);
        std::string previous1, previous2;
        const auto boundary = [&] {
            if (!previous1.empty())
                state_->bigrams[previous1].try_emplace("", 0);
            if (!previous2.empty())
                state_->trigrams[previous2][previous1].try_emplace("", 0);
            previous1.clear();
            previous2.clear();
        };
        tokenize(
            state_->history[i],
            [&](std::string word) {
                state_->local[word] += weight;
                state_->localTotal += weight;
                if (!previous1.empty())
                    state_->bigrams[previous1][word] += weight;
                if (!previous2.empty())
                    state_->trigrams[previous2][previous1][word] += weight;
                previous2 = std::move(previous1);
                previous1 = std::move(word);
            },
            boundary);
        boundary();
    }
}

std::size_t WordCompletion::State::complete(std::string_view before, std::string_view prefix,
                                            std::span<RankedWord> ranks,
                                            std::span<std::string> output, bool extensionsOnly) {
    if (output.empty() || !validUtf8(before))
        return 0;
    if (prefix.empty()) {
        if (extensionsOnly || before.empty() || (before.back() != ' ' && before.back() != '\t'))
            return 0;
    } else if (!validWord(prefix)) {
        return 0;
    }
    ranks = ranks.first(std::min(ranks.size(), output.size()));
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
    if (prefix.empty()) {
        extend(previous2, previous1, maxPredictedWords, output.front());
        return !output.front().empty();
    }
    const auto folded = lower(prefix);
    const auto *bigram = findContext(bigrams, previous1);
    const auto *trigram = this->trigram(previous2, previous1);
    const auto scale = [](const Counts *counts, double weight) {
        double total = 0;
        if (counts)
            for (const auto &[word, count] : *counts)
                total += count;
        return total ? weight / total : 0;
    };
    const double bigramScale = scale(bigram, 0.65);
    const double trigramScale = scale(trigram, 1.5);
    const double localScale = localTotal ? 0.2 / localTotal : 0;
    const auto count = [](const Counts *counts, std::string_view word) {
        if (counts) {
            const auto found = counts->find(word);
            if (found != counts->end())
                return found->second;
        }
        return 0.0;
    };
    static const Counts empty;
    const auto &generalCounts = loaded.load(std::memory_order_acquire) ? prior : empty;
    auto general = generalCounts.lower_bound(folded);
    auto recent = local.lower_bound(folded);
    std::size_t ranked = 0;
    while ((general != generalCounts.end() && general->first.starts_with(folded)) ||
           (recent != local.end() && recent->first.starts_with(folded))) {
        const bool useGeneral = general != generalCounts.end() &&
                                general->first.starts_with(folded) &&
                                (recent == local.end() || general->first <= recent->first);
        const std::string_view word = useGeneral ? general->first : recent->first;
        double score = 0;
        if (general != generalCounts.end() && general->first == word)
            score += (general++)->second;
        if (recent != local.end() && recent->first == word)
            score += (recent++)->second * localScale;
        if (extensionsOnly && word == folded)
            continue;
        score += count(bigram, word) * bigramScale + count(trigram, word) * trigramScale;
        // Candidates arrive in lexical order, so tied scores retain that order.
        auto slot = ranks.begin();
        while (slot != ranks.begin() + ranked && slot->score >= score)
            ++slot;
        if (slot == ranks.end())
            continue;
        const auto oldEnd = ranks.begin() + ranked;
        if (ranked < ranks.size())
            ++ranked;
        std::move_backward(slot, std::min(oldEnd, ranks.begin() + ranked - 1),
                           ranks.begin() + ranked);
        *slot = {word, score};
    }
    if (!ranked)
        return 0;
    bool uppercase = true;
    bool cased = false;
    for (const char *p = prefix.data(), *end = p + prefix.size(); p < end;
         p = g_utf8_next_char(p)) {
        const auto ch = g_utf8_get_char(p);
        if (g_unichar_islower(ch))
            uppercase = false;
        cased = cased || g_unichar_isupper(ch) || g_unichar_islower(ch);
    }
    for (std::size_t i = 0; i < ranked; ++i) {
        auto &result = output[i];
        result.assign(prefix);
        result += ranks[i].word.substr(folded.size());
        if (!extensionsOnly) {
            const auto remaining = maxPredictedWords - (result.size() > prefix.size());
            extend(previous1, ranks[i].word, remaining, result);
            if (result.size() == prefix.size()) {
                result.clear();
                return 0;
            }
        }
        if (uppercase && cased) {
            auto *upper = g_utf8_strup(result.data() + prefix.size(),
                                       static_cast<gssize>(result.size() - prefix.size()));
            result.replace(prefix.size(), std::string::npos, upper);
            g_free(upper);
        }
    }
    return ranked;
}

std::string WordCompletion::complete(std::string_view before, std::string_view prefix) {
    RankedWord rank;
    std::string result;
    state_->complete(before, prefix, {&rank, 1}, {&result, 1}, false);
    return result;
}

std::size_t WordCompletion::completions(std::string_view before, std::string_view prefix,
                                        std::span<std::string> output) {
    std::array<RankedWord, 8> ranks;
    return state_->complete(before, prefix, ranks, output, true);
}

} // namespace blast
