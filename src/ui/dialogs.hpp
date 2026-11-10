#pragma once

#include <render/scene/input_area.h>
#include <render/scene/node.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

class InputDispatcher;

namespace blast {
struct Model;

class Dialogs final : public Node {
  public:
    Dialogs(Model &, std::function<void(std::string)> execute);
    ~Dialogs() override;
    void open(std::string_view name);
    bool handleKey(const InputArea::KeyData &);
    bool active() const;
    bool settingsOpen() const;
    void setInputDispatcher(InputDispatcher &);
    void sync(float width, float height, float scale);
    void remapBuffer(std::string_view oldId, std::string_view newId);
    void remapPane(std::uint64_t oldId, std::uint64_t newId);
    void dismiss();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace blast
