#pragma once

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

  private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace blast
