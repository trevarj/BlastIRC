#pragma once

#include <memory>
#include <string>
#include <string_view>

namespace blast {

class WordCompletion {
  public:
    explicit WordCompletion(std::string path = {});
    ~WordCompletion();
    bool ready() const;
    // Full, strictly longer match; the immutable view lives until this object is destroyed.
    std::string_view complete(std::string_view prefix) const;

  private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace blast
