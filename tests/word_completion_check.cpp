#include "word_completion.hpp"

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

using namespace std::chrono_literals;

namespace {
void await(const blast::WordCompletion &completion) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!completion.ready() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    assert(completion.ready() && "wordlist worker did not finish");
}

struct TemporaryDirectory {
    char path[64] = "/tmp/blastirc-word-completion-XXXXXX";
    TemporaryDirectory() {
        const auto created = mkdtemp(path);
        assert(created);
    }
    ~TemporaryDirectory() { std::filesystem::remove_all(path); }
};
} // namespace

int main() {
    TemporaryDirectory directory;
    const auto path = std::filesystem::path(directory.path) / "words";
    const std::string longest(128, 'x');
    {
        std::ofstream output(path, std::ios::binary);
        output << "zebra\naccommodations\naccommodation\naa\naaa\naa\naaa\n"
                  "café\n€uro\n𐐀word\nsolo\n\n"
                  "bad space\nbad\tcontrol\n";
        output << "broken\xc0\xaf\n"; // Overlong UTF-8.
        output << "surrogate\xed\xa0\x80\n";
        output << "outofrange\xf4\x90\x80\x80\n";
        output << "truncated\xe2\x82\n";
        output << "continuation\x80\n";
        output << "nul" << '\0' << "word\n";
        output << longest << '\n' << std::string(129, 'y') << '\n';
        output << "tailword"; // A final newline is not required.
        assert(output.good());
    }
    blast::WordCompletion completion(path.string());
    await(completion);
    assert(completion.complete("accom") == "accommodation");
    assert(completion.complete("accommodation") == "accommodations");
    assert(completion.complete("accommodations").empty());
    assert(completion.complete("aa") == "aaa"); // Deduplicate before skipping the exact word.
    assert(completion.complete("a") == "aa");   // Sort independent of file order.
    assert(completion.complete("solo").empty());
    assert(completion.complete("caf") == "café");
    assert(completion.complete("café").empty());
    assert(completion.complete("€") == "€uro");
    assert(completion.complete("𐐀") == "𐐀word");
    assert(completion.complete(std::string(127, 'x')) == longest);
    assert(completion.complete(longest).empty());
    assert(completion.complete(std::string(128, 'y')).empty());
    for (const auto prefix : {"bad", "broken", "surrogate", "outofrange", "truncated",
                              "continuation", "nul", "missing"})
        assert(completion.complete(prefix).empty());
    assert(completion.complete("").empty());
    assert(completion.complete("two words").empty());
    assert(completion.complete("caf\xc3").empty()); // Partial UTF-8 is not a word prefix.
    assert(completion.complete(std::string(129, 'x')).empty());
    assert(completion.complete("tail") == "tailword");

    const auto retained = completion.complete("accom");
    std::filesystem::remove(path);
    assert(completion.complete("ze") == "zebra"); // No per-query file reads.
    assert(retained == "accommodation");

    blast::WordCompletion unavailable(path.string());
    await(unavailable);
    assert(unavailable.complete("accom").empty());
    {
        std::ofstream empty(path);
    }
    blast::WordCompletion empty(path.string());
    await(empty);
    assert(empty.complete("accom").empty());

    {
        std::ofstream oversized(path, std::ios::binary | std::ios::trunc);
        oversized << "accommodation\n";
        oversized.seekp(16 * 1024 * 1024);
        oversized << '\n';
        assert(oversized.good());
    }
    blast::WordCompletion oversized(path.string());
    await(oversized);
    assert(oversized.complete("accom").empty()); // Reject the whole file, not a partial index.

    // Destruction joins an outstanding load, including failed opens.
    blast::WordCompletion destroyedWhileLoading((path.string() + ".missing"));
}
