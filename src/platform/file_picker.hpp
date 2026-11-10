#pragma once

#include "upload.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace blast::platform {

enum class FilePickerStatus { Picked, Cancelled, Error };

struct PickedFile {
    std::string path;
    std::string name;
    std::string mime;
    std::uint64_t size = 0;
    UploadFileIdentity identity; // Original O_NOFOLLOW open/fstat; never replace during review.
};

struct FilePickerResult {
    FilePickerStatus status = FilePickerStatus::Error;
    PickedFile file;
    std::string error;
};

// Main-thread API; the worker owns its GLib context and never calls the UI.
class FilePicker {
  public:
    FilePicker();
    ~FilePicker();
    FilePicker(const FilePicker &) = delete;
    FilePicker &operator=(const FilePicker &) = delete;
    bool open(std::string title = "Choose a file", std::string parentWindow = {});
    void cancel();
    bool busy() const;
    std::optional<FilePickerResult> poll();

  private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace blast::platform
