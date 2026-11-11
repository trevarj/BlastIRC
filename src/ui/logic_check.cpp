// Run: meson test -C build workspace_logic --print-errorlogs
#include "logic.hpp"
#include <cassert>
#include <cmath>
#include <limits>

static void friendIdentity() {
    blast::Model model;
    model.settings.friends = {"[Alice]^", "BOB"};
    assert(blast::isFriend(model, "missing", "{alice}~"));
    assert(blast::isFriend(model, "missing", "bob"));
    assert(!blast::isFriend(model, "missing", ""));
    assert(!blast::isFriend(model, "missing", "other"));
    auto &network = model.networks["network"];
    network.caseMapping = "ascii";
    assert(!blast::isFriend(model, "network", "{alice}~"));
    assert(blast::isFriend(model, "network", "[alice]^"));
    network.caseMapping = "strict-rfc1459";
    assert(blast::isFriend(model, "network", "{alice}^"));
    assert(!blast::isFriend(model, "network", "{alice}~"));
    network.caseMapping = "rfc1459";
    assert(blast::isFriend(model, "network", "{alice}~"));
    model.settings.friends.clear();
    assert(!blast::isFriend(model, "network", "[Alice]^"));
}

static void sidebarNavigation() {
    using namespace blast;
    using namespace blast::ui_logic;
    Buffer first, duplicate, query;
    first.id = "one/#same";
    first.server = "First network";
    first.target = "#same";
    first.unread = 3;
    duplicate = first;
    duplicate.id = "two/#same";
    duplicate.server = "Second network";
    duplicate.unread = 0;
    query.id = "two/alex";
    query.server = "Second network";
    query.target = "alex";
    query.kind = "query";
    query.unread = 1;
    assert(sidebarMatches(first, "SAME", SidebarFilter::All));
    assert(sidebarMatches(duplicate, "same", SidebarFilter::All));
    assert(sidebarMatches(first, "first", SidebarFilter::Unread));
    assert(!sidebarMatches(duplicate, "first", SidebarFilter::All));
    assert(!sidebarMatches(duplicate, "second", SidebarFilter::Unread));
    assert(sidebarMatches(query, "second", SidebarFilter::Dms));
    assert(!sidebarMatches(first, "first", SidebarFilter::Dms));
    assert(!sidebarMatches(query, "missing", SidebarFilter::Dms));
    assert(sidebarMatches(duplicate, "", SidebarFilter::All));
    assert(sidebarMatches(first, "libera", SidebarFilter::All, "Libera Chat"));
    assert(sidebarMatches(first, "first", SidebarFilter::All, "Libera Chat"));
    assert(sidebarMatches(first, "same", SidebarFilter::All, "Libera Chat"));
    assert(!sidebarMatches(first, "libera", SidebarFilter::Dms, "Libera Chat"));
    assert(!sidebarMatches(first, "missing", SidebarFilter::All, "Libera Chat"));
    query.archived = true;
    assert(!sidebarMatches(query, "", SidebarFilter::All));
    assert(!sidebarMatches(query, "", SidebarFilter::Unread));
    assert(!sidebarMatches(query, "", SidebarFilter::Dms));
    assert(sidebarMatches(query, "person", SidebarFilter::Archived) == false);
    assert(sidebarMatches(query, "alex", SidebarFilter::Archived));
    assert(query.unread == 1);
    Model archivedModel;
    archivedModel.buffers.emplace(query.id, query);
    first.kind = "channel";
    archivedModel.buffers.emplace(first.id, first);
    const auto hints = avyHints(archivedModel, query.server);
    assert(hints.empty());
    assert(avyHints(archivedModel, first.server).size() == 1);
    query.kind = "search";
    assert(!sidebarMatches(query, "", SidebarFilter::Archived));
    query.kind = "server";
    assert(!sidebarMatches(query, "", SidebarFilter::Archived));
    query.kind = "query";
    query.archived = false;
    assert(!sidebarMatches(query, "", SidebarFilter::Archived));
    Message older;
    older.nick = "mia";
    older.text = "older";
    first.messages.push_back(older);
    Message latest;
    latest.nick = "alex";
    latest.text = "\x02"
                  "bold\x02\n \t猫✨\x03"
                  "04,12 red\x0f \x04"
                  "AABBCC,001122hex\x01";
    first.messages.push_back(latest);
    std::string preview;
    assert(!sidebarPreview(archivedModel, first, "", preview) &&
           preview == "alex: bold 猫✨ red hex");
    assert(sidebarPreview(archivedModel, first, "\tshared\n draft✨", preview) &&
           preview == "Draft: shared draft✨");
    archivedModel.settings.fools.insert("alex");
    archivedModel.settings.bufferFools[first.id] = false;
    sidebarPreview(archivedModel, first, "", preview);
    assert(preview == "mia: older");
    archivedModel.settings.bufferFools[first.id] = true;
    sidebarPreview(archivedModel, first, "", preview);
    assert(preview == "alex: bold 猫✨ red hex");
    archivedModel.settings.fools.clear();
    // Redaction and echo reconciliation mutate the same message without changing count or ID.
    first.messages.back().deleted = true;
    assert(!sidebarPreview(archivedModel, first, "", preview) && preview == "mia: older");
    first.messages.back().deleted = false;
    first.messages.back().presence = true;
    sidebarPreview(archivedModel, first, "", preview);
    assert(preview == "mia: older");
    first.messages.back().presence = false;
    first.messages.back().own = true;
    first.messages.back().pending = true;
    first.messages.back().text = "queued";
    sidebarPreview(archivedModel, first, "", preview);
    assert(preview == "[Pending] You: queued");
    first.messages.back().pending = false;
    sidebarPreview(archivedModel, first, "", preview);
    assert(preview == "You: queued");
    first.messages.back().action = true;
    sidebarPreview(archivedModel, first, "", preview);
    assert(preview == "* alex queued");
    Message presence;
    presence.presence = true;
    first.messages.clear();
    first.messages.push_back(older);
    for (int i = 0; i < 63; ++i)
        first.messages.push_back(presence);
    sidebarPreview(archivedModel, first, "", preview);
    assert(preview == "mia: older");
    first.messages.push_back(presence);
    sidebarPreview(archivedModel, first, "", preview);
    assert(preview == "No messages yet");
    preview.clear();
    appendPreview(std::string(511, 'a') + "猫", preview);
    assert(preview == std::string(511, 'a'));
    preview.clear();
    appendPreview(std::string(2048, '\x02') + "outside scan", preview);
    assert(preview.empty());
    appendPreview(std::string(2047, '\x02') + "猫", preview);
    assert(preview.empty());
    first.messages.clear();
    older.nick.assign(1024, 'x');
    older.text = "not beyond output bound";
    first.messages.push_back(older);
    sidebarPreview(archivedModel, first, "", preview);
    assert(preview.size() <= 512 && preview.ends_with(':'));
    first.kind = "server";
    sidebarPreview(archivedModel, first, "", preview);
    assert(preview == "Server activity");
    first.kind = "search";
    sidebarPreview(archivedModel, first, "", preview);
    assert(preview == "Search results");
    assert(sidebarPreview(archivedModel, first, "saved draft", preview) &&
           preview == "Draft: saved draft");
}

static void focusKeyRemapping() {
    using namespace blast::ui_logic;
    std::string key = "buffer:old";
    remapBufferFocusKey(key, "old", "long-name");
    assert(key == "buffer:long-name");
    remapBufferFocusKey(key, "long-name", "x");
    assert(key == "buffer:x");
    remapBufferFocusKey(key, "x", "X");
    assert(key == "buffer:X");
    remapBufferFocusKey(key, "X", "X");
    assert(key == "buffer:X");
    key = "3:old:message:3:old:reply";
    remapBufferFocusKey(key, "old", "long-name");
    assert(key == "9:long-name:message:3:old:reply");
    key = "9:long-name:message:abc:action:3:old";
    remapBufferFocusKey(key, "long-name", "x");
    assert(key == "1:x:message:abc:action:3:old");
    remapBufferFocusKey(key, "x", "X");
    assert(key == "1:X:message:abc:action:3:old");
    for (std::string unrelated :
         {"buffer:old-more", "buffer:other", "4:old:message", "prefix:3:old:message", "unknown"}) {
        key = unrelated;
        remapBufferFocusKey(key, "old", "new");
        assert(key == unrelated);
    }
    for (std::string_view role : {"composer:", "buffer-tool:", "transcript:"}) {
        key = std::string(role) + "1:opaque:1:composer:1:";
        remapPaneFocusKey(key, 1, 100);
        assert(key == std::string(role) + "100:opaque:1:composer:1:");
        remapPaneFocusKey(key, 100, 2);
        assert(key == std::string(role) + "2:opaque:1:composer:1:");
        remapPaneFocusKey(key, 2, 2);
        assert(key == std::string(role) + "2:opaque:1:composer:1:");
        key = std::string(role) + "10:suffix";
        remapPaneFocusKey(key, 1, 7);
        assert(key == std::string(role) + "10:suffix");
        key = std::string(role) + "1";
        remapPaneFocusKey(key, 1, 7);
        assert(key == std::string(role) + "1");
    }
    for (std::string unrelated :
         {"unknown:1:suffix", "buffer:1:suffix", "prefix:composer:1:suffix", ""}) {
        key = unrelated;
        remapPaneFocusKey(key, 1, 7);
        assert(key == unrelated);
    }
}

static void jumpHintKeyboard() {
    using blast::ui_logic::jumpHintKey;
    assert(jumpHintKey('A') == 'a');
    assert(jumpHintKey('Z') == 'z');
    assert(jumpHintKey('a') == 'a');
    assert(jumpHintKey('0') == '0');
    assert(jumpHintKey('9') == '9');
    assert(!jumpHintKey(0x01000444)); // Cyrillic ф is not a displayed ASCII hint.
    assert(!jumpHintKey(0x01000634)); // Arabic ش is not a displayed ASCII hint.
    assert(!jumpHintKey(0));
    assert(!jumpHintKey(0xfe51)); // Dead acute must not become a hint.
    assert(!jumpHintKey(0xff0d));
    assert(!jumpHintKey('@'));
}

static void avyJumpHints() {
    using namespace blast;
    using namespace blast::ui_logic;
    auto add = [](Model &model, std::string id, std::string target, std::string kind = "channel",
                  std::string server = "selected") {
        Buffer buffer;
        buffer.id = id;
        buffer.target = std::move(target);
        buffer.kind = std::move(kind);
        buffer.server = std::move(server);
        model.buffers.emplace(std::move(id), std::move(buffer));
    };
    auto prefixFree = [](const std::vector<JumpHint> &hints) {
        for (size_t i = 0; i < hints.size(); ++i)
            for (size_t j = 0; j < hints.size(); ++j)
                if (i != j)
                    assert(!hints[j].keys.starts_with(hints[i].keys));
    };
    Model unique;
    add(unique, "03", "猫_BETA", "query");
    add(unique, "02", "#--9lives");
    add(unique, "01", "#!ALpha");
    add(unique, "04", "#other", "channel", "other");
    add(unique, "05", "server", "server");
    add(unique, "06", "search", "search");
    const auto initials = avyHints(unique, "selected");
    assert(initials.size() == 3);
    assert(initials[0].buffer == "01" && initials[0].keys == "a");
    assert(initials[1].buffer == "02" && initials[1].keys == "9");
    assert(initials[2].buffer == "03" && initials[2].keys == "b");
    prefixFree(initials);
    const auto other = avyHints(unique, "other");
    assert(other.size() == 1 && other[0].buffer == "04" && other[0].keys == "o");
    assert(avyHints(unique, "missing").empty());
    assert(avyHints(Model{}, "selected").empty());

    Model crowded;
    for (int i = 81; i >= 0; --i)
        add(crowded, "channel/" + std::to_string(i), "#Alpha" + std::to_string(i));
    add(crowded, "query/z", "Zoe", "query");
    const auto collisions = avyHints(crowded, "selected");
    assert(collisions.size() == 83);
    assert(
        std::is_sorted(collisions.begin(), collisions.end(),
                       [](const JumpHint &a, const JumpHint &b) { return a.buffer < b.buffer; }));
    // 82 colliding initials need three base-nine suffix digits, not one or two.
    constexpr std::string_view home = "asdfghjkl";
    for (size_t i = 0; i < 82; ++i) {
        assert(collisions[i].keys.size() == 4 && collisions[i].keys.front() == 'a');
        assert(collisions[i].keys.find_first_not_of(home, 1) == std::string::npos);
    }
    assert(collisions.front().keys == "aaaa" && collisions[81].keys == "asaa");
    assert(collisions.back().keys == "z");
    prefixFree(collisions);
    Model reordered;
    for (auto it = crowded.buffers.rbegin(); it != crowded.buffers.rend(); ++it)
        reordered.buffers.emplace(it->first, it->second);
    const auto repeated = avyHints(reordered, "selected");
    assert(repeated.size() == collisions.size());
    for (size_t i = 0; i < repeated.size(); ++i) {
        assert(repeated[i].buffer == collisions[i].buffer);
        assert(repeated[i].keys == collisions[i].keys);
    }

    Model fallback;
    for (size_t i = 0; i < 11; ++i)
        add(fallback, "fallback/" + std::to_string(10 + i),
            i % 3 == 0   ? ""
            : i % 3 == 1 ? "#!!!"
                         : "#猫✨");
    add(fallback, "natural", "#Alpha");
    const auto symbols = avyHints(fallback, "selected");
    assert(symbols.size() == 12);
    for (size_t i = 0; i < 11; ++i)
        assert(symbols[i].keys.front() == home[i % home.size()]);
    assert(symbols[0].keys == "aa" && symbols[9].keys == "as");
    assert(symbols[1].keys == "sa" && symbols[10].keys == "ss");
    assert(symbols[2].keys == "d" && symbols.back().keys == "ad");
    prefixFree(symbols);

    const auto explicitDefault = avyHints(unique, "selected", 0);
    for (size_t i = 0; i < initials.size(); ++i)
        assert(explicitDefault[i].keys == initials[i].keys);
    for (char reserved : std::string_view("ab9")) {
        const auto safe = avyHints(unique, "selected", reserved);
        assert(safe.size() == initials.size());
        for (size_t i = 0; i < safe.size(); ++i) {
            assert(safe[i].buffer == initials[i].buffer);
            assert(safe[i].keys.find(reserved) == std::string::npos);
            if (initials[i].keys.front() != reserved)
                assert(safe[i].keys.front() == initials[i].keys.front());
        }
        prefixFree(safe);
        const auto safeSymbols = avyHints(fallback, "selected", reserved);
        assert(safeSymbols.size() == symbols.size());
        for (size_t i = 0; i < safeSymbols.size(); ++i) {
            assert(safeSymbols[i].buffer == symbols[i].buffer);
            assert(safeSymbols[i].keys.find(reserved) == std::string::npos);
        }
        prefixFree(safeSymbols);
    }
    for (int i = 0; i < 65; ++i)
        add(crowded, "delta/" + std::to_string(i), "#Delta" + std::to_string(i));
    const auto baseEight = avyHints(crowded, "selected", 'a');
    assert(baseEight.size() == crowded.buffers.size());
    for (const auto &hint : baseEight) {
        assert(hint.keys.find('a') == std::string::npos);
        // 65 D-initial targets exceed two digits in the filtered eight-key alphabet.
        if (hint.buffer.starts_with("delta/"))
            assert(hint.keys.front() == 'd' && hint.keys.size() == 4);
    }
    prefixFree(baseEight);
}

static void transcriptIdentity() {
    using blast::Message;
    using blast::ui_logic::messageKey;
    Message first;
    first.id = "server-message-1";
    first.label = "shared-history-request";
    first.history = true;
    Message second = first;
    second.id = "server-message-2";
    assert(messageKey(first) != messageKey(second));
    first.own = second.own = true;
    assert(messageKey(first) != messageKey(second));
    first.history = second.history = false;
    first.own = second.own = false;
    assert(messageKey(first) != messageKey(second)); // Labeled utility responses also share labels.
    first.id = "local:missing-server-id-1";
    second.id = "local:missing-server-id-2";
    assert(messageKey(first) != messageKey(second));

    Message outgoing;
    outgoing.id = "local:pending-send";
    outgoing.label = "outbound-send";
    outgoing.own = outgoing.pending = true;
    const auto pendingKey = messageKey(outgoing);
    outgoing.id = "authoritative-server-message";
    outgoing.pending = false;
    assert(messageKey(outgoing) == pendingKey);

    blast::Buffer transcript;
    transcript.messages.push_back(first);
    outgoing.id = "local:pending-send";
    outgoing.pending = true;
    transcript.messages.push_back(outgoing);
    using blast::ui_logic::readTarget;
    assert(readTarget(transcript) == &transcript.messages.front());
    const auto priorReadId = readTarget(transcript)->id;
    auto &live = transcript.messages.back();
    live.pending = false;
    live.id = "authoritative-server-message";
    live.time = "2026-11-05T01:57:16.768Z";
    assert(readTarget(transcript) == &live && live.id != priorReadId);
    assert(messageKey(live) == pendingKey);
    const auto priorTime = readTarget(transcript)->time;
    live.time = "2026-11-05T01:57:16.985Z";
    assert(readTarget(transcript)->time != priorTime && messageKey(live) == pendingKey);
    live.deleted = true; // Deleted rows remain read positions, matching Application policy.
    blast::Message presence;
    presence.presence = true;
    transcript.messages.push_back(presence);
    outgoing.pending = true;
    transcript.messages.push_back(outgoing);
    assert(readTarget(transcript) == &live);
    transcript.messages.front().presence = true;
    live.pending = true;
    assert(!readTarget(transcript));
}

static void unreadMentions() {
    using namespace blast;
    using namespace blast::ui_logic;
    Model model;
    Buffer live;
    live.server = "network";
    live.unread = 1;
    live.readMarker = "2026-11-05T01:57:16.500Z";
    Message message;
    message.id = "equal";
    message.nick = "Speaker";
    message.mention = true;
    message.time = live.readMarker;
    message.timestamp = irc::parseTimestampParts(message.time)->seconds;
    live.messages.push_back(message);
    assert(!hasUnreadMention(model, live));
    live.messages.back().time = "2026-11-05T01:57:16.500000001Z";
    assert(hasUnreadMention(model, live)); // Fractions, not just whole seconds.
    live.messages.back().time = "2026-11-05T01:57:16.499999999Z";
    assert(!hasUnreadMention(model, live));

    message.time.clear();
    message.timestamp = 0;
    live.messages = {message, message};
    live.messages.front().id = "local:boundary";
    live.messages.back().id = "new";
    live.readMarker = "local:local:boundary";
    assert(hasUnreadMention(model, live));
    live.messages.back().mention = false;
    assert(!hasUnreadMention(model, live)); // The boundary itself is never unread.
    live.readMarker = "local:pruned";
    assert(hasUnreadMention(model, live)); // A missing local boundary admits the tail.
    live.readMarker = "local:boundary";    // Also accept already-local message IDs.
    assert(!hasUnreadMention(model, live));
    live.readMarker.clear();

    live.messages = {message};
    live.messages.front().own = true;
    assert(!hasUnreadMention(model, live));
    live.messages.front().own = false;
    live.messages.front().presence = true;
    assert(!hasUnreadMention(model, live));
    live.messages.front().presence = false;
    model.settings.fools.insert("speaker");
    assert(!hasUnreadMention(model, live));
    model.settings.foolUnread = true;
    assert(hasUnreadMention(model, live));
    model.settings.foolUnread = false;
    model.settings.fools.clear();

    Buffer retained = live;
    retained.unread = 0; // Retained counters/markers are not live read state.
    retained.readMarker = "local:new";
    retained.messages = {message, message};
    retained.messages.front().id = "boundary";
    retained.messages.back().id = "new";
    live.messages.clear(); // Retained mentions can be outside the loaded window.
    live.readMarker = "local:boundary";
    const auto collect = [&] {
        std::vector<std::string> ids;
        forEachUnreadMention(model, live, retained, [&](const Message &entry) {
            ids.push_back(entry.id);
            return true;
        });
        return ids;
    };
    assert(collect() == std::vector<std::string>{"new"});
    live.readMarker = "local:missing";
    assert((collect() == std::vector<std::string>{"boundary", "new"}));
    live.unread = 0;
    assert(collect().empty()); // Mark-read hides retained mentions immediately.
}

static void draftRecovery() {
    using namespace blast::ui_logic;
    Editor editor;
    std::string published, modelDraft = "saved 猫✨";
    assert(reconcileDraft(editor, published, modelDraft));
    assert(editor.text == modelDraft && published == modelDraft);
    assert(editor.cursor == modelDraft.size() && editor.anchor == editor.cursor);

    // Submit clears the shared model; a disconnected rejection restores it afterward.
    const auto submitted = editor.text;
    editor.remember();
    editor.text.clear();
    editor.cursor = editor.anchor = 0;
    assert(!reconcileDraft(editor, published, modelDraft));
    assert(modelDraft.empty() && published.empty());
    modelDraft = submitted;
    assert(reconcileDraft(editor, published, modelDraft));
    assert(editor.text == submitted && modelDraft == submitted && published == submitted);

    // An asynchronous rejection prepends to the last publication, not stale native widget text.
    editor.text = "new 猫 draft";
    editor.cursor = editor.anchor = editor.text.size();
    assert(!reconcileDraft(editor, published, modelDraft));
    editor.insert(" ✨");
    const auto fresh = editor.text;
    editor.selection(7, 4, 7);
    const auto cursor = editor.cursor, anchor = editor.anchor;
    modelDraft = "failed send\n" + published;
    assert(reconcileDraft(editor, published, modelDraft));
    assert(editor.text == "failed send\n" + fresh);
    assert(modelDraft == editor.text && published == editor.text);
    assert(editor.mark && editor.cursor == cursor + 12 && editor.anchor == anchor + 12);
    const auto undoCount = editor.undo.size();
    assert(!reconcileDraft(editor, published, modelDraft));
    assert(editor.undo.size() == undoCount && editor.mark);
    std::string kill;
    editor.command("undo", kill);
    assert(editor.text == fresh);
    assert(!reconcileDraft(editor, published, modelDraft) && modelDraft == fresh);
    modelDraft.clear(); // A later pending echo must not erase newly typed text.
    assert(!reconcileDraft(editor, published, modelDraft) && modelDraft == fresh);

    // Existing recovery is never truncated to the ordinary admission limit.
    editor.text = std::string(40000, 'a') + "猫";
    editor.cursor = editor.anchor = editor.text.size();
    reconcileDraft(editor, published, modelDraft);
    editor.insert("✨ newer");
    const auto largeFresh = editor.text;
    const std::string recovery = std::string(40000, 'b') + "🤝";
    modelDraft = recovery + "\n" + published;
    assert(reconcileDraft(editor, published, modelDraft));
    const auto combined = recovery + "\n" + largeFresh;
    assert(combined.size() > composerLimit && editor.text == combined);
    assert(modelDraft == combined && published == combined);
    assert(editor.cursor == combined.size() && editor.anchor == editor.cursor);
    assert(!composerInsertAllowed(editor, "x"));
    assert(composerEditAllowed(combined, std::string_view(combined).substr(1)));
    assert(!composerEditAllowed(combined, combined));
    assert(!composerEditAllowed(combined, combined + "x"));
    assert(!composerEditAllowed(combined, "new text" + combined.substr(20)));
    editor.command("undo", kill);
    assert(editor.text == largeFresh);
    Editor ordinary;
    ordinary.text.assign(composerLimit - 2, 'x');
    ordinary.cursor = ordinary.anchor = ordinary.text.size();
    assert(composerInsertAllowed(ordinary, "x"));
    assert(!composerInsertAllowed(ordinary, "é"));
    ordinary.insert("x");
    assert(!composerInsertAllowed(ordinary, "x"));
    ordinary.selection(ordinary.text.size(), 0, ordinary.text.size());
    assert(composerInsertAllowed(ordinary, "replacement"));
}

static void clipboardAndSelection() {
    using namespace blast::ui_logic;
    assert(composerTextValid("猫✨\n\té"));
    assert(composerTextValid("\x02"
                             "bold\x02\x03"
                             "04\x0f"));
    for (const auto &text : {std::string("nul\0text", 8), std::string("\xc0\x80"),
                             std::string("\xed\xa0\x80"), std::string("\xf4\x90\x80\x80"),
                             std::string("\xe2\x82"), std::string("\x7f"), std::string("\r")})
        assert(!composerTextValid(text));
    assert(clipboardNewlines("one\r\ntwo\r\n猫") == "one\ntwo\n猫");
    assert(!composerTextValid(clipboardNewlines("one\rtwo")));
    Editor editor;
    editor.text = "a猫éz";
    editor.cursor = 6;
    editor.anchor = 1;
    editor.mark = true;
    editor.insert("✨");
    assert(editor.text == "a✨z");
    std::string kill;
    editor.command("undo", kill);
    assert(editor.text == "a猫éz" && editor.cursor == 6 && editor.anchor == 1 && editor.mark);
    // Recovered oversized drafts admit deletion, not same-size replacement or one more codepoint.
    const std::string recovered(composerLimit + 20, 'x');
    assert(composerEditAllowed(recovered, recovered.substr(10)));
    assert(!composerEditAllowed(recovered, "猫" + recovered.substr(3)));
}

static void editingCommands() {
    using namespace blast::ui_logic;
    Editor editor;
    std::string kill = "retained";
    editor.text = "aé猫";
    editor.cursor = editor.anchor = editor.text.size();
    const std::array<size_t, 4> stops{0, 1, 4, 7};
    assert(editor.transpose(stops));
    assert(editor.text == "a猫é" && editor.cursor == 7 && kill == "retained");
    assert(editor.undo.size() == 1);
    editor.command("undo", kill);
    assert(editor.text == "aé猫" && editor.cursor == 7);
    editor.cursor = editor.anchor = 1;
    assert(editor.transpose(stops));
    assert(editor.text == "éa猫" && editor.cursor == 4);
    editor.text = "ab\ncd";
    editor.cursor = editor.anchor = 2;
    const std::array<size_t, 6> lines{0, 1, 2, 3, 4, 5};
    assert(editor.transpose(lines) && editor.text == "ba\ncd");
    editor.cursor = editor.anchor = 3;
    assert(!editor.transpose(lines) && editor.text == "ba\ncd");
    editor.cursor = 1;
    editor.anchor = 0;
    editor.mark = true;
    assert(!editor.transpose(lines) && editor.text == "ba\ncd" && editor.mark);
    editor = {};
    editor.text = "a\né猫z";
    editor.cursor = 7;
    editor.anchor = 2;
    editor.mark = true;
    editor.command("backward-kill-line", kill);
    assert(editor.text == "a\nz" && kill == "é猫" && editor.cursor == 2 && !editor.mark);
    assert(editor.undo.size() == 1);
    editor.command("undo", kill);
    assert(editor.text == "a\né猫z" && editor.cursor == 7 && editor.anchor == 2 && editor.mark);
    editor.mark = false;
    editor.cursor = editor.anchor = 2;
    editor.command("backward-kill-line", kill);
    assert(editor.text == "aé猫z" && kill == "\n" && editor.cursor == 1);
    editor.cursor = editor.anchor = 0;
    const auto historySize = editor.undo.size();
    editor.command("backward-kill-line", kill);
    assert(editor.text == "aé猫z" && kill == "\n" && editor.undo.size() == historySize);
}

static void messageBindings() {
    using namespace blast::ui_logic;
    using Bindings = std::map<std::string, std::string>;
    auto migrate = [](Bindings &bindings) {
        migrateMessageBindings(
            bindings, [&](std::string_view name) { return bindings.contains(std::string(name)); });
    };
    Bindings stock{{"previous-line", " C-P "},
                   {"next-line", "c-n"},
                   {"end-of-line", "C-e"},
                   {"switch-buffer", "C-x b"},
                   {"jump-channel", "C-x j"}};
    migrate(stock);
    assert(stock.at("previous-line").empty() && stock.at("next-line").empty() &&
           stock.at("end-of-line").empty());
    assert(stock.at("message-prev") == "C-p" && stock.at("message-next") == "C-n" &&
           stock.at("message-reply") == "C-r" && stock.at("message-react") == "C-e");
    assert(stock.at("switch-buffer") == "C-x b" && stock.at("jump-channel") == "C-x j");
    const auto migrated = stock;
    migrate(stock);
    assert(stock == migrated);

    Bindings custom{{"previous-line", "M-p"}, {"next-line", ""},        {"end-of-line", "END"},
                    {"message-prev", ""},     {"message-next", "M-F8"}, {"message-reply", "C-e"},
                    {"user-command", "C-r x"}};
    migrate(custom);
    assert(custom.at("previous-line") == "M-p" && custom.at("next-line").empty() &&
           custom.at("end-of-line") == "END");
    assert(custom.at("message-prev").empty() && custom.at("message-next") == "M-F8" &&
           custom.at("message-reply") == "C-e" && custom.at("message-react").empty());
    assert(custom.at("user-command") == "C-r x");
    Bindings rebound{{"previous-line", "C-p"}, {"next-line", "C-n"},    {"end-of-line", "C-e"},
                     {"message-prev", ""},     {"message-next", "M-n"}, {"message-react", ""}};
    migrate(rebound);
    assert(rebound.at("previous-line") == "C-p" && rebound.at("next-line") == "C-n" &&
           rebound.at("end-of-line") == "C-e" && rebound.at("message-prev").empty() &&
           rebound.at("message-next") == "M-n" && rebound.at("message-react").empty());
    for (const auto *chord : {"C-e", " c-E  a "}) {
        Bindings overlap{{"end-of-line", "C-e"}, {"message-react", chord}};
        migrate(overlap);
        assert(overlap.at("end-of-line").empty() && overlap.at("message-react") == chord);
        const auto resolved = overlap;
        migrate(overlap);
        assert(overlap == resolved);
    }
    Bindings crossAction{{"end-of-line", "C-e"}, {"message-react", ""}, {"message-reply", "C-e a"}};
    migrate(crossAction);
    assert(crossAction.at("end-of-line").empty() && crossAction.at("message-react").empty() &&
           crossAction.at("message-reply") == "C-e a");
    Bindings customOverlap{{"end-of-line", "M-e"}, {"message-react", "M-e"}};
    migrate(customOverlap);
    assert(customOverlap.at("end-of-line") == "M-e" && customOverlap.at("message-react") == "M-e");
    for (const auto *chord : {"C-p", " c-P  x "}) {
        Bindings conflict{{"user-command", chord}};
        migrate(conflict);
        assert(conflict.at("message-prev").empty() && conflict.at("user-command") == chord);
    }
    Bindings replyConflict{{"user-command", "C-r"}};
    migrate(replyConflict);
    assert(replyConflict.at("message-reply").empty());

    // Storage overlays saved values onto defaults before migrating.
    Bindings loaded{{"message-prev", "C-p"},  {"message-next", "C-n"},  {"message-reply", "C-r"},
                    {"message-react", "C-e"}, {"previous-line", "C-p"}, {"user-command", "C-n x"}};
    migrateMessageBindings(loaded, [](std::string_view) { return false; });
    assert(loaded.at("previous-line").empty() && loaded.at("message-prev") == "C-p" &&
           loaded.at("message-next").empty() && loaded.at("message-reply") == "C-r" &&
           loaded.at("message-react") == "C-e");
}

int main() {
    using namespace blast::ui_logic;
    friendIdentity();
    messageBindings();
    editingCommands();
    assert(fuzzy("br", "blastirc") > fuzzy("zz", "blastirc"));
    assert(canonical("C-x  B") == "c-x b");
    assert(bindingConflicts("c-x j", "c-x j"));
    assert(bindingConflicts("C-X J", "\t c-x  j \n"));
    assert(bindingConflicts("C-x Enter", "c-x RETURN"));
    assert(bindingConflicts("c-x j", "c-x j a"));
    assert(bindingConflicts("c-x j a", "c-x j"));
    assert(bindingConflicts("c-x", "c-x j"));
    assert(!bindingConflicts("", "c-x j"));
    assert(!bindingConflicts("c-x j", " \t\n"));
    assert(!bindingConflicts("", ""));
    assert(!bindingConflicts("c-x j", "c-x jx"));
    assert(!bindingConflicts("c-x jx", "c-x j"));
    assert(!bindingConflicts("c-x j", "c-x b"));
    assert(blast::irc::clientTagAllowed("", "+reply"));
    assert(!blast::irc::clientTagAllowed("*", "+reply"));
    assert(blast::irc::clientTagAllowed("*,-reply", "+reply"));
    assert(!blast::irc::clientTagAllowed("draft/*", "+draft/react"));
    assert(blast::irc::clientTagAllowed("draft/*,-draft/react", "+draft/react"));
    assert(userScale(std::numeric_limits<float>::quiet_NaN()) == 1.f);
    assert(userScale(.5f) == .7f && userScale(3.f) == 2.f);
    sidebarNavigation();
    avyJumpHints();
    jumpHintKeyboard();
    focusKeyRemapping();
    transcriptIdentity();
    unreadMentions();
    draftRecovery();
    clipboardAndSelection();
    // A genuine prepend removes the old first row's date/author headers. Its body,
    // including a below-top body point while the loading slot is visible, must not move.
    const double oldBodyTop = 32 + 36 + 18;
    const double prependedBodyTop = 32 + 1200;
    const double prependedScroll =
        anchoredBodyScroll(0, oldBodyTop, prependedBodyTop, 20, 20, false);
    assert(prependedBodyTop - prependedScroll == oldBodyTop);
    assert(anchoredBodyScroll(90, oldBodyTop, prependedBodyTop, 20, 20, false) ==
           prependedBodyTop + 4);
    assert(anchoredBodyScroll(60, 0, 0, 80, 20, true) == 15);
    assert(anchoredBodyScroll(15, 0, 0, 20, 80, true) == 60);
    assert(anchoredBodyScroll(120, 0, 0, 80, 20, true) == 60);
    assert(anchoredBodyScroll(80, 0, 0, 80, 20, true) == 20);
    assert(anchoredBodyScroll(86, 0, 0, 80, 80, true) == 86);
    assert(anchoredBodyScroll(26, 0, 100, 20, 80, true) == 186);
    assert(anchoredBodyScroll(0, 0, 100, 80, 20, true) == 100);
    assert(anchoredBodyScroll(0, 8, 16, 80, 20, true) == 8);
    assert(anchoredBodyScroll(1, 0, 0, 0, 20, true) == 21);
    for (const auto &token : {"0", "15", "16", "98", "99", "999", "-1", "", "\xff"}) {
        size_t offset = 0;
        auto number = colorNumber(token, offset);
        if (number)
            assert(*number <= 99 && *number % 16 < 16 && offset <= 2);
        else
            assert(offset == 0);
    }
    size_t offset = 0;
    assert(colorNumber("12,34", offset) == 12 && offset == 2);
    ++offset;
    assert(colorNumber("12,34", offset) == 34 && offset == 5);
    Editor e;
    e.text = "aé猫 z";
    e.cursor = e.text.size();
    std::string kill;
    e.command("backward-word", kill);
    assert(e.cursor == 7);
    e.command("backward-char", kill);
    assert(e.cursor == 6);
    e.command("backward-char", kill);
    assert(e.cursor == 3);
    e.command("set-mark", kill);
    e.command("backward-char", kill);
    e.command("kill-region", kill);
    assert(e.text == "a猫 z" && kill == "é");
    e.command("yank", kill);
    assert(e.text == "aé猫 z");
    e.command("undo", kill);
    assert(e.text == "a猫 z");
    e.text = "one\ntwo";
    e.cursor = 3;
    e.command("kill-line", kill);
    assert(e.text == "onetwo" && kill == "\n");
    e.text = "éa\nxyz";
    e.cursor = 2;
    e.mark = false;
    e.command("next-line", kill);
    assert(e.cursor == 5);
    e.command("previous-line", kill);
    assert(e.cursor == 2);
    Editor noop;
    noop.insert("x");
    noop.command("delete-char", kill);
    noop.command("kill-line", kill);
    noop.command("kill-word", kill);
    noop.command("undo", kill);
    assert(noop.text.empty());
    e.text = "12345";
    e.cursor = 4;
    e.anchor = 1;
    e.mark = true;
    assert(e.insertedSize("ab") == 4);
    e.insert("ab");
    assert(e.text == "1ab5");
    // Consumer sequence: these events can all arrive before the next rendered frame.
    Editor typing;
    typing.insert("abc");
    typing.insert("XYZ");
    typing.command("beginning-of-line", kill);
    typing.command("delete-char", kill);
    typing.command("end-of-line", kill);
    typing.command("backward-delete", kill);
    assert(typing.text == "bcXY" && typing.cursor == 4);
    typing.selection(1, 1, 1);
    typing.command("set-mark", kill);
    typing.selection(1, 1, 1);
    assert(typing.mark && typing.anchor == 1);
    typing.selection(3, 1, 3);
    assert(typing.mark && typing.anchor == 1 && typing.cursor == 3);
    typing.selection(2, 2, 2);
    assert(!typing.mark && typing.cursor == 2 && typing.anchor == 2);
    typing.selection(4, 2, 4);
    typing.selection(0, 0, 0);
    assert(!typing.mark && typing.anchor == 0);
}
