#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace blast {

class WordCompletion {
  public:
    explicit WordCompletion(std::string path = {});
    ~WordCompletion();
    bool ready() const;
    void setHistory(std::span<const std::string> newestFirst);
    std::string complete(std::string_view before, std::string_view prefix);
    // Ranked extensions only, at most eight; complete() also considers the typed word.
    std::size_t completions(std::string_view before, std::string_view prefix,
                            std::span<std::string> output);

  private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace blast
