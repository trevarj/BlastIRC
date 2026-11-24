#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace blast {

struct SpellRange {
    std::size_t begin = 0, end = 0;
};

struct SpellResult {
    std::uint64_t ticket = 0;
    std::string language, error;
    std::vector<SpellRange> misspelled;
};

class Spellcheck {
  public:
    static constexpr std::string_view allLanguages = "*";

    Spellcheck();
    ~Spellcheck();
    void request(std::uint64_t ticket, std::string text, std::string language,
                 std::vector<SpellRange> excluded = {});
    std::optional<SpellResult> poll();
    std::vector<std::string> suggest(std::string_view word, std::string_view language);
    void setDictionaryPaths(const std::vector<std::string> &paths);
    static std::vector<std::string> languages(const std::vector<std::string> &paths = {});
    static bool validDictionaryPath(std::string_view path) {
        return path.starts_with('/') && path.find_first_of({"\0\r\n", 3}) == std::string_view::npos;
    }
    static bool hasDictionaryPair(std::string_view path);

  private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace blast
