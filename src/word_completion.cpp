#include "word_completion.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <fstream>
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

bool validWord(std::string_view word) {
    if (word.empty() || word.size() > maxWordBytes)
        return false;
    for (std::size_t i = 0; i < word.size();) {
        const auto first = static_cast<unsigned char>(word[i++]);
        if (first < 0x80) {
            if (first <= 0x20 || first == 0x7f)
                return false;
            continue;
        }
        const int tail = first >= 0xc2 && first <= 0xdf   ? 1
                         : first >= 0xe0 && first <= 0xef ? 2
                         : first >= 0xf0 && first <= 0xf4 ? 3
                                                          : -1;
        if (tail < 0 || i + tail > word.size())
            return false;
        std::uint32_t code = first & (0x7f >> (tail + 1));
        for (int n = 0; n < tail; ++n) {
            const auto byte = static_cast<unsigned char>(word[i++]);
            if ((byte & 0xc0) != 0x80)
                return false;
            code = (code << 6) | (byte & 0x3f);
        }
        if ((tail == 1 && code < 0x80) || (tail == 2 && code < 0x800) ||
            (tail == 3 && code < 0x10000) || code > 0x10ffff ||
            (code >= 0xd800 && code <= 0xdfff) || (code >= 0x80 && code <= 0x9f))
            return false;
    }
    return true;
}
} // namespace

struct WordCompletion::State {
    std::atomic<bool> loaded{false};
    std::vector<std::string> words;
    std::jthread worker;

    explicit State(std::string path)
        : worker([this, path = std::move(path)] {
              try {
                  std::ifstream input(path, std::ios::binary);
                  std::string line;
                  line.reserve(maxWordBytes);
                  bool oversized = false;
                  std::size_t bytes = 0;
                  const auto append = [&] {
                      if (!oversized && validWord(line))
                          words.push_back(line);
                      line.clear();
                      oversized = false;
                  };
                  char byte;
                  while (input.get(byte)) {
                      if (++bytes > maxFileBytes) {
                          words.clear();
                          break;
                      }
                      if (byte == '\n')
                          append();
                      else if (line.size() < maxWordBytes)
                          line.push_back(byte);
                      else
                          oversized = true;
                  }
                  if (bytes <= maxFileBytes) {
                      if (input.bad())
                          words.clear();
                      else
                          append();
                  }
                  std::sort(words.begin(), words.end());
                  words.erase(std::unique(words.begin(), words.end()), words.end());
              } catch (...) {
                  words.clear();
              }
              loaded.store(true, std::memory_order_release);
          }) {}
};

WordCompletion::WordCompletion(std::string path)
    : state_(std::make_unique<State>(path.empty() ? BLASTIRC_WORDLIST_FILE : std::move(path))) {}

WordCompletion::~WordCompletion() = default;

bool WordCompletion::ready() const { return state_->loaded.load(std::memory_order_acquire); }

std::string_view WordCompletion::complete(std::string_view prefix) const {
    if (!ready() || !validWord(prefix))
        return {};
    const auto &words = state_->words;
    auto match = std::lower_bound(words.begin(), words.end(), prefix);
    if (match != words.end() && *match == prefix)
        ++match;
    if (match != words.end() && match->starts_with(prefix))
        return *match;
    return {};
}

} // namespace blast
