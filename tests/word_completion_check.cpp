#include "word_completion.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {
void await(const blast::WordCompletion &completion) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!completion.ready() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    assert(completion.ready() && "frequency worker did not finish");
}

struct TemporaryDirectory {
    char path[64] = "/tmp/blastirc-word-completion-XXXXXX";
    TemporaryDirectory() { assert(mkdtemp(path)); }
    ~TemporaryDirectory() { std::filesystem::remove_all(path); }
};
} // namespace

int main() {
    TemporaryDirectory directory;
    const auto path = std::filesystem::path(directory.path) / "frequencies.tsv";
    const auto missing = path.string() + ".missing";
    const std::string longest(128, 'x');
    {
        std::ofstream output(path, std::ios::binary);
        output << "review\t200\nremember\t300\n"
                  "accommodations\t500\naccommodation\t200\n"
                  "café\t300\nÉclair\t300\n𐐀word\t300\n"
                  "dupeword\t100\ndupeword\t600\nduperival\t150\n"
                  "ambiguous\t300\nambiguity\t310\n"
                  "the\t200\nthat\t225\nthere\t250\nthen\t275\n"
                  "broken\tNaN\nnegative\t-1\noverbin\t1001\n"
                  "fraction\t100.5\nextra\t200\textra\nmissingbin\t\n"
                  "oldformat\nbad space\t200\n";
        output << "overlong\xc0\xaf\t200\n"
                  "surrogate\xed\xa0\x80\t200\n"
                  "outofrange\xf4\x90\x80\x80\t200\n"
                  "truncated\xe2\x82\t200\n"
                  "continuation\x80\t200\n";
        output << "nul" << '\0' << "word\t200\n";
        output << longest << "\t300\n" << std::string(129, 'y') << "\t200\n";
        output << "tailword\t300";
        assert(output.good());
    }
    blast::WordCompletion completion(path.string());
    await(completion);
    assert(completion.complete("", "re") == "review"); // Frequency, not alphabetical order.
    assert(completion.complete("", "accom") == "accommodation");
    assert(completion.complete("", "accommodation").empty()); // Exact words are stop candidates.
    assert(completion.complete("", "dupe") == "dupeword");
    assert(completion.complete("", "dupew") == "dupeword");
    assert(completion.complete("", "t") == "the"); // Broad cold prefix: no majority required.
    assert(completion.complete("", "the").empty());
    assert(completion.complete("", "ambi") == "ambiguous"); // Close runner-up is not a veto.
    assert(completion.complete("", "caf") == "café");
    assert(completion.complete("", "CAF") == "CAFÉ");
    assert(completion.complete("", "Caf") == "Café");
    assert(completion.complete("", "cAf") == "cAfé");
    assert(completion.complete("", "écl") == "éclair");
    assert(completion.complete("", "𐐀") == "𐐀WORD");
    assert(completion.complete("", std::string(127, 'x')) == longest);
    assert(completion.complete("", longest).empty());
    assert(completion.complete("", std::string(128, 'y')).empty());
    for (const auto prefix :
         {"broken", "negative", "overbin", "fraction", "extra", "missingbin", "oldformat", "bad",
          "overlong", "surrogate", "outofrange", "truncated", "continuation", "nul", "missing"})
        assert(completion.complete("", prefix).empty());
    assert(completion.complete("", "").empty());
    assert(completion.complete("", "two words").empty());
    assert(completion.complete("", "caf\xc3").empty());
    assert(completion.complete("", std::string(129, 'x')).empty());
    assert(completion.complete("bad\xff", "re").empty());
    assert(completion.complete("", "tail") == "tailword");

    const std::vector<std::string> contexts = {"can you review", "please do remember",
                                               "can you review", "please do remember"};
    completion.setHistory(contexts);
    assert(completion.complete("can you ", "re") == "review");
    assert(completion.complete("please do ", "re") == "remember");
    assert(completion.complete("can you ", "r") == "review");
    assert(completion.complete("please do ", "r") == "remember");
    assert(completion.complete("can you ", "re") == "review"); // Same prefix, changed context.
    completion.setHistory(contexts);
    assert(completion.complete("can you ", "re") == "review");
    assert(completion.complete("please do. ", "re") != "remember");
    assert(completion.complete("please do\n", "re") != "remember");
    const auto retained = completion.complete("can you ", "re");
    std::filesystem::remove(path);
    assert(completion.complete("", "tail") == "tailword"); // No query-time file reads.
    assert(retained == "review");

    blast::WordCompletion local(missing);
    // Local vocabulary works without waiting for the asynchronous prior.
    local.setHistory(std::vector<std::string>{"quartzite"});
    assert(local.complete("", "Quar") == "Quartzite");
    await(local);
    assert(local.complete("", "quar") == "quartzite");
    local.setHistory(std::vector<std::string>{"'quartzite'", "‘Éclairunique’"});
    assert(local.complete("", "quar") == "quartzite");
    assert(local.complete("", "écl") == "éclairunique");
    local.setHistory(std::vector<std::string>{"review--remember"});
    assert(local.complete("", "rev") == "review");
    assert(local.complete("", "rem") == "remember");
    local.setHistory(std::vector<std::string>{"remember remember remember", "can you--review"});
    assert(local.complete("", "re") == "remember");
    assert(local.complete("can you ", "re") == local.complete("", "re"));
    local.setHistory(std::vector<std::string>{"remember remember remember", "can 'you' review"});
    assert(local.complete("", "re") == "remember");
    assert(local.complete("can you ", "re") == local.complete("", "re"));
    local.setHistory(contexts);
    assert(local.complete("can you ", "re") == "review");
    assert(local.complete("please do ", "re") == "remember");
    const auto withoutContext = local.complete("", "re");
    assert(withoutContext == "review" || withoutContext == "remember");
    assert(local.complete("", "r") == withoutContext);
    local.setHistory(std::vector<std::string>{"review review review", "remember"});
    assert(local.complete("", "re") == "review");
    std::vector<std::string> recency(32, "remember");
    recency.front() = "review";
    for (std::size_t i = 1; i < 16; ++i)
        recency[i] = "review";
    local.setHistory(recency);
    assert(local.complete("", "re") == "review");
    std::reverse(recency.begin(), recency.end());
    local.setHistory(recency);
    assert(local.complete("", "re") == "remember");
    local.setHistory(std::vector<std::string>{"remember remember remember", "can you. review"});
    assert(local.complete("", "re") == "remember");
    assert(local.complete("can you ", "re") == local.complete("", "re")); // No sentence crossing.
    local.setHistory(std::vector<std::string>{"remember remember remember", "can you", "review"});
    assert(local.complete("", "re") == "remember");
    assert(local.complete("can you ", "re") == local.complete("", "re")); // No message crossing.
    local.setHistory(std::vector<std::string>{"can you review", "please you remember"});
    assert(local.complete("can you ", "re") == "review"); // Trigram beats shared bigram.
    assert(local.complete("please you ", "re") == "remember");
    local.setHistory(
        std::vector<std::string>{"goodword bad\xff", std::string(512, ' ') + "hiddenword"});
    assert(local.complete("", "good").empty());
    assert(local.complete("", "hidden").empty());
    local.setHistory(std::vector<std::string>{std::string(511, ' ') + "éclair"});
    assert(local.complete("", "écl").empty()); // Byte bound cannot split UTF-8 into a token.
    std::vector<std::string> bounded(32, "ordinary");
    bounded.push_back("excludedword");
    local.setHistory(bounded);
    assert(local.complete("", "excluded").empty());
    local.setHistory(std::span<const std::string>{});
    assert(local.complete("can you ", "re").empty());

    {
        std::ofstream output(path);
    }
    blast::WordCompletion empty(path.string());
    await(empty);
    assert(empty.complete("", "re").empty());
    {
        std::ofstream oversized(path, std::ios::binary | std::ios::trunc);
        oversized << "accommodation\t200\n";
        oversized.seekp(16 * 1024 * 1024);
        oversized << '\n';
        assert(oversized.good());
    }
    blast::WordCompletion oversized(path.string());
    await(oversized);
    assert(oversized.complete("", "accom").empty()); // Reject entire oversized file.
    oversized.setHistory(std::vector<std::string>{"localword"});
    assert(oversized.complete("", "local") == "localword");
    blast::WordCompletion destroyedWhileLoading(missing);
}
