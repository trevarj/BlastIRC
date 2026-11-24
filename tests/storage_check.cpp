// Standalone: compile with storage.cpp, upload.cpp, irc/protocol.cpp, irc/sts.cpp and ui/logic.cpp;
// link sqlite3, libcurl, toml++ and pthread (or use the storage-check Meson target).
#include "irc/protocol.hpp"
#include "irc/sts.hpp"
#include "storage.hpp"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <regex>
#include <sqlite3.h>
#include <sys/stat.h>
#include <toml++/toml.hpp>
#include <unistd.h>

using namespace blast;
namespace fs = std::filesystem;
static string contents(const fs::path &file) {
    std::ifstream input(file);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
static void mode(const fs::path &file, unsigned expected) {
    struct stat value{};
    assert(::stat(file.c_str(), &value) == 0);
    assert((value.st_mode & 0777) == expected);
}
static void presentation(const Settings &actual, const Settings &expected) {
    assert(actual.sendMotion == expected.sendMotion && actual.sendReveal == expected.sendReveal &&
           actual.channelMessageLayout == expected.channelMessageLayout &&
           actual.directMessageLayout == expected.directMessageLayout &&
           actual.messageLayouts == expected.messageLayouts && actual.uiFont == expected.uiFont &&
           actual.messageFont == expected.messageFont);
    assert(actual.scale == expected.scale && actual.messageScale == expected.messageScale);
    assert(actual.uiFontWeight == expected.uiFontWeight &&
           actual.messageFontWeight == expected.messageFontWeight);
    assert(actual.incomingMotion == expected.incomingMotion &&
           actual.incomingReveal == expected.incomingReveal &&
           actual.incomingDurationMs == expected.incomingDurationMs &&
           actual.foolDisplay == expected.foolDisplay && actual.foolUnread == expected.foolUnread &&
           actual.messageWrapColumns == expected.messageWrapColumns);
    assert(actual.linkPreviews == expected.linkPreviews &&
           actual.imagePreviews == expected.imagePreviews &&
           actual.mediaPreviews == expected.mediaPreviews &&
           actual.previewMaxHeight == expected.previewMaxHeight &&
           actual.previewMediaMaxMiB == expected.previewMediaMaxMiB &&
           actual.previewMediaMaxSeconds == expected.previewMediaMaxSeconds);
    assert(actual.previewAllowLocal == expected.previewAllowLocal);
}
static Message message(string id, int64_t timestamp, string text = "hello") {
    Message value;
    value.id = std::move(id);
    value.timestamp = timestamp;
    value.text = std::move(text);
    value.nick = "alice";
    return value;
}
static void timestampFunction(sqlite3 *db) {
    assert(sqlite3_create_function_v2(
               db, "server_nanoseconds", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS,
               nullptr,
               [](sqlite3_context *context, int, sqlite3_value **values) {
                   const auto *text = sqlite3_value_text(values[0]);
                   const auto timestamp =
                       text ? irc::parseTimestampParts(
                                  {reinterpret_cast<const char *>(text),
                                   static_cast<size_t>(sqlite3_value_bytes(values[0]))})
                            : std::nullopt;
                   sqlite3_result_int(context, timestamp ? timestamp->nanoseconds : 0);
               },
               nullptr, nullptr, nullptr) == SQLITE_OK);
}
static int untrustedSchema(sqlite3 *db, char **, const sqlite3_api_routines *) {
    return sqlite3_db_config(db, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr);
}
static void sql(const fs::path &file, const char *command) {
    sqlite3 *db = nullptr;
    assert(sqlite3_open(file.c_str(), &db) == SQLITE_OK);
    timestampFunction(db);
    const int result = sqlite3_exec(db, command, nullptr, nullptr, nullptr);
    if (result != SQLITE_OK)
        std::cerr << sqlite3_errmsg(db) << '\n';
    assert(result == SQLITE_OK);
    assert(sqlite3_close(db) == SQLITE_OK);
}
static void dropHistoryOrderSchema(const fs::path &database) {
    sql(database,
        "DROP INDEX messages_buffer_order; DROP INDEX messages_order; DROP INDEX messages_time;"
        "DROP INDEX messages_chat_activity;"
        "ALTER TABLE messages DROP COLUMN tie_order;"
        "ALTER TABLE messages DROP COLUMN tie_order_confirmed;"
        "ALTER TABLE messages DROP COLUMN mention_nick;"
        "CREATE INDEX messages_buffer_order ON "
        "messages(buffer,sort_time,server_nanoseconds(time),seq);"
        "CREATE INDEX messages_order ON messages(sort_time,server_nanoseconds(time),seq);"
        "CREATE INDEX messages_time ON "
        "messages(buffer,time,sort_time,server_nanoseconds(time),seq);");
}
static void dropHistorySchema(const fs::path &database) {
    sql(database,
        "DROP TABLE history_gaps; DROP TABLE history_state; "
        "DROP TABLE history_discovery; ALTER TABLE closed_buffers DROP COLUMN discarded_at;");
    dropHistoryOrderSchema(database);
}
static int64_t scalar(const fs::path &file, const char *query) {
    sqlite3 *db = nullptr;
    sqlite3_stmt *statement = nullptr;
    assert(sqlite3_open(file.c_str(), &db) == SQLITE_OK);
    timestampFunction(db);
    assert(sqlite3_prepare_v2(db, query, -1, &statement, nullptr) == SQLITE_OK);
    assert(sqlite3_step(statement) == SQLITE_ROW);
    const int64_t value = sqlite3_column_int64(statement, 0);
    assert(sqlite3_finalize(statement) == SQLITE_OK);
    assert(sqlite3_close(db) == SQLITE_OK);
    return value;
}
static Buffer sendBuffer(string id) {
    Buffer buffer;
    buffer.id = std::move(id);
    const auto slash = buffer.id.find('/');
    buffer.server = buffer.id.substr(0, slash);
    buffer.target = buffer.id.substr(slash + 1);
    buffer.title = buffer.target;
    return buffer;
}
static void chatActivityMetadata(int64_t now) {
    Storage storage;
    Model model;
    storage.load(model);
    model.settings.retentionDays = 0;
    model.settings.maxMessages = 10000;
    const string id = "Libera/#chat-activity", peer = "Libera/chat-activity-query";
    model.buffers[id] = sendBuffer(id);
    model.buffers[peer] = sendBuffer(peer);
    model.buffers[peer].kind = "query";
    model.settings.pinnedBuffers = {peer, id};
    storage.save(model);
    auto chat = message("same-source-id", now);
    chat.time = "2023-11-14T22:13:20.100000001Z";
    int64_t first = 0, second = 0;
    storage.append(id, chat, "", 0, false, &model.buffers.at(id), &first);
    chat.own = chat.action = true;
    storage.append(peer, chat, "", 0, false, &model.buffers.at(peer), &second);
    assert(first > 0 && second > first);
    auto presence = message("", now + 1);
    presence.presence = true;
    for (int i = 0; i < 205; ++i) {
        presence.id = "chat-presence-" + std::to_string(i);
        storage.append(id, presence);
    }
    Model loaded;
    storage.load(loaded);
    const auto &room = loaded.buffers.at(id), &query = loaded.buffers.at(peer);
    assert(room.messages.size() == 1 && room.messages.front().id == "same-source-id" &&
           !room.messages.front().presence);
    assert(room.lastChatTimestamp == now && room.lastChatSequence == first &&
           room.lastChatTime == chat.time && query.lastChatSequence == second &&
           chatActivityBefore(query, room));
    assert(loaded.settings.pinnedBuffers == model.settings.pinnedBuffers);
    // Fractions normalize offsets and short spellings, then use durable sequence/ID.
    Buffer a = room, b = room;
    a.id = "a";
    b.id = "b";
    assert(chatActivityBefore(a, b) && !chatActivityBefore(b, a));
    b.lastChatTime = "2023-11-14T23:13:20.100000002+01:00";
    assert(chatActivityBefore(b, a));
    b.lastChatTime = "2023-11-14T22:13:20.1Z";
    assert(chatActivityBefore(a, b));
    Buffer empty;
    empty.id = "0";
    assert(chatActivityBefore(room, empty));
    // Seeding does not modify retained rows or their per-source identities.
    Buffer reopened = sendBuffer(id);
    storage.seedChatActivity(reopened);
    assert(reopened.lastChatSequence == first &&
           storage.message(id, chat.id)->storageSequence == first &&
           storage.message(peer, chat.id)->storageSequence == second);
    // Even a crash between durable close and preferences rewrite cannot resurrect a pin.
    storage.closeBuffer(model.buffers.at(id));
    storage.saveSettings(model.settings);
    storage.load(loaded);
    assert(loaded.settings.pinnedBuffers == vector<string>{peer} && !loaded.buffers.contains(id));
}
static void contentHistoryWindows(int64_t now) {
    Storage storage;
    Model model;
    storage.load(model);
    storage.clearHistory();
    model.settings.retentionDays = 0;
    model.settings.maxMessages = 10000;
    const string id = "Libera/#content-window", peer = "Libera/content-window-query",
                 server = "Libera/*server*", empty = "Libera/#presence-only";
    for (const auto &source : {id, peer, server, empty})
        model.buffers[source] = sendBuffer(source);
    model.buffers[peer].kind = "query";
    model.buffers[server].kind = "server";
    storage.save(model);
    auto pending = message("local:old-window-echo", now - 1);
    pending.own = pending.pending = true;
    storage.append(id, pending);
    pending.pending = false;
    storage.update(id, pending);
    auto presence = message("", now);
    presence.presence = true;
    for (int index = 0; index < 405; ++index) {
        auto row = message("content-" + std::to_string(index), now + index);
        row.action = index == 10;
        row.own = index == 13;
        if (index == 11)
            row.nick = "notice";
        row.deleted = index == 12;
        storage.append(id, row);
        for (int event = 0; event < 3; ++event) {
            presence.id = "interleaved-" + std::to_string(index) + "-" + std::to_string(event);
            presence.timestamp = now + index;
            storage.append(id, presence);
        }
    }
    for (int index = 0; index < 250; ++index) {
        presence.id = "tail-" + std::to_string(index);
        presence.timestamp = now + 405 + index;
        storage.append(id, presence);
        storage.append(peer, presence);
        storage.append(server, presence);
    }
    storage.append(empty, presence);
    auto queryRow = message("query-content", now);
    queryRow.action = true;
    storage.append(peer, queryRow);
    const auto all = storage.history(id, -1);
    assert(all.size() == 1871 && std::count_if(all.begin(), all.end(), [](const auto &row) {
                                     return row.presence;
                                 }) == 1465);
    Model loaded;
    storage.load(loaded);
    const auto &startup = loaded.buffers.at(id).messages;
    assert(startup.size() == 201 && startup.front().id == pending.id &&
           startup.front().sendStatus == SendStatus::Unknown && startup.front().pending &&
           startup[1].id == "content-205" && startup.back().id == "content-404");
    const auto latest = storage.history(id);
    assert(latest.size() == 200 && latest.front().id == startup[1].id &&
           latest.back().id == startup.back().id);
    const auto older = storage.history(id, 200, latest.front().id);
    assert(older.size() == 200 && older.front().id == "content-5" &&
           older.back().id == "content-204" && older[5].action && older[6].nick == "notice" &&
           older[7].deleted && older[7].text.empty() && older[8].own &&
           older[8].sendStatus == SendStatus::Confirmed);
    const auto oldest = storage.history(id, 200, older.front().id);
    assert(oldest.size() == 5 && oldest.front().id == "content-0" &&
           oldest.back().id == "content-4" && storage.history(id, 200, oldest.front().id).empty());
    assert(storage.history(id, 1, pending.id).empty());
    assert(storage.history(id, 1, "interleaved-205-0").front().id == "content-205");
    assert(storage.history(id, 0).empty() && storage.history(id, -2).empty());
    assert(storage.history(peer).size() == 1 && loaded.buffers.at(peer).messages.size() == 1 &&
           loaded.buffers.at(peer).messages.front().action);
    assert(storage.history(empty).empty() && loaded.buffers.at(empty).messages.empty() &&
           storage.history(empty, -1).size() == 1);
    assert(storage.history(server).size() == 200 && storage.history(server).front().presence &&
           loaded.buffers.at(server).messages.size() == 200 &&
           loaded.buffers.at(server).messages.front().presence &&
           storage.history(server, -1).size() == 250);
    assert(storage.history(id, -1).size() == all.size());
    storage.clearHistory();
}

static void fractionalHistory(const fs::path &database) {
    constexpr int64_t second = 1700000000; // 2023-11-14T22:13:20Z
    const string id = "Libera/#fractional-history";
    auto timed = [&](string key, string time) {
        auto row = message(std::move(key), second);
        row.time = std::move(time);
        return row;
    };
    const auto schema = scalar(database, "PRAGMA user_version");
    // Exercise real Storage reads, indexed writes and migration with schema trust disabled.
    assert(sqlite3_auto_extension(reinterpret_cast<void (*)()>(untrustedSchema)) == SQLITE_OK);
    {
        Storage storage;
        Model model;
        storage.load(model);
        storage.clearHistory();
        model.settings.retentionDays = 0;
        model.settings.maxMessages = 1000;
        model.buffers[id] = sendBuffer(id);
        storage.save(model);
        // Arrival order opposes server time at the actual 200-row page/window boundary.
        auto recentPending = timed("late", "2023-11-14T22:13:20.900Z");
        recentPending.own = recentPending.pending = true;
        storage.append(id, recentPending);
        auto pending = message("local:fractional-pending", second - 1);
        pending.own = pending.pending = true;
        storage.append(id, pending);
        for (int index = 0; index < 200; ++index)
            storage.append(id, timed("early-" + std::to_string(index), "2023-11-14T22:13:20.100Z"));
        const auto page = storage.history(id);
        assert(page.size() == 200 && page.front().id == "early-0" && page.back().id == "early-199");
        const auto older = storage.history(id, 200, page.front().id);
        assert(older.empty());
        assert(storage.history(id, 1, "late").front().id == "early-199");
        assert(storage.history(id, 1, "2023-11-15T00:13:20.5+02:00").front().id == "early-199");
        storage.load(model);
        const auto &loaded = model.buffers.at(id).messages;
        assert(loaded.size() == 202 && loaded.front().id == pending.id && loaded.front().pending &&
               loaded[1].id == "early-0" && loaded.back().id == "late" && loaded.back().own &&
               loaded.back().pending);
    }
    {
        Storage storage;
        Model model;
        storage.load(model);
        const auto &loaded = model.buffers.at(id).messages;
        assert(loaded.size() == 202 && loaded.front().id == "local:fractional-pending" &&
               loaded.front().pending && loaded[1].id == "early-0" && loaded.back().id == "late" &&
               loaded.back().own && loaded.back().pending);
        const auto page = storage.history(id);
        assert(page.size() == 200 && page.front().id == "early-0" && page.back().id == "early-199");
        const auto older = storage.history(id, 200, page.front().id);
        assert(older.empty());
        storage.clearHistory();

        storage.append(id, timed("late", "2023-11-14T22:13:20.900Z"));
        storage.append(id, timed("next-nanosecond", "2023-11-14T22:13:20.100000001Z"));
        storage.append(id, timed("offset", "2023-11-14T23:13:20.1000000009+01:00"));
        storage.append(id, timed("early", "2023-11-14T22:13:20.1Z"));
        storage.append(id, timed("invalid", "2023-11-14T22:13:20.900Zjunk"));
        storage.append(id, timed("invalid-calendar", "2023-02-29T22:13:20.800Z"));
        storage.append(id, timed("zero", "2023-11-14T22:13:20.000Z"));
        storage.append(id, timed("no-fraction", "2023-11-14T22:13:20Z"));
        const auto ordered = storage.history(id);
        assert(ordered.size() == 8 && ordered[0].id == "invalid" &&
               ordered[1].id == "invalid-calendar" && ordered[2].id == "zero" &&
               ordered[3].id == "no-fraction" && ordered[4].id == "offset" &&
               ordered[5].id == "early" && ordered[6].id == "next-nanosecond" &&
               ordered[7].id == "late");
        assert(storage.history(id, 1, "early").front().id == "offset");
        assert(storage.history(id, 1, "2023-11-14T22:13:20.100000001+00:00").front().id == "early");
        // Exact stored time keeps its historical sequence boundary, unlike a new UTC spelling.
        assert(storage.history(id, 1, "2023-11-14T22:13:20.1Z").front().id == "offset");
        assert(storage.history(id, 1, "2023-11-14T22:13:20.000000000+00:00").empty());
        for (const auto *invalid : {"2023-11-14T22:13:20.5junk", "2023-02-29T22:13:20.5Z",
                                    "0000-01-01T00:00:00.5Z", "2023-11-14T22:13:60.5Z"}) {
            bool failed = false;
            try {
                storage.history(id, 1, invalid);
            } catch (const std::runtime_error &) {
                failed = true;
            }
            assert(failed);
        }
        auto unknown = timed("unknown-fraction", "2023-11-14T22:13:20.9junk");
        unknown.timestamp = 0;
        storage.append("Libera/#invalid-fraction", unknown);
        assert(storage.message("Libera/#invalid-fraction", unknown.id)->timestamp == 0);
        assert(scalar(database, "SELECT timestamp=0 AND sort_time=received_at FROM messages "
                                "WHERE id='unknown-fraction'") == 1);
        const auto receiptOrder = scalar(database, "SELECT sort_time FROM messages "
                                                   "WHERE id='unknown-fraction'");
        assert(receiptOrder > 0 &&
               storage.message("Libera/#invalid-fraction", unknown.id)->orderingTimestamp ==
                   receiptOrder);
        const auto unknownHistory = storage.history("Libera/#invalid-fraction", -1);
        assert(unknownHistory.size() == 1 && unknownHistory[0].timestamp == 0 &&
               unknownHistory[0].orderingTimestamp == receiptOrder);
        storage.load(model);
        assert(model.buffers.at("Libera/#invalid-fraction").messages[0].orderingTimestamp ==
               receiptOrder);
        const auto &reloaded = model.buffers.at(id).messages;
        assert(reloaded.size() == ordered.size());
        for (size_t index = 0; index < ordered.size(); ++index)
            assert(reloaded[index].id == ordered[index].id);
        storage.append(id, message("previous-second", second - 1));
        const auto combined = storage.history(id);
        assert(combined.size() == ordered.size() + 1 && combined.front().id == "previous-second");
        for (size_t boundary = 0; boundary < combined.size(); ++boundary) {
            const auto prior = storage.history(id, 200, combined[boundary].id);
            assert(prior.size() == boundary);
            for (size_t index = 0; index < boundary; ++index)
                assert(prior[index].id == combined[index].id);
        }
        const string opaqueBuffer = id + "-opaque";
        const string timestampId = "2023-11-14T22:13:20.500Z";
        const auto opaqueSource = sendBuffer(opaqueBuffer);
        storage.append(opaqueBuffer, timed("time-match", timestampId), "", 0, false, &opaqueSource);
        storage.append(opaqueBuffer, timed("middle", "2023-11-14T22:13:20.800Z"));
        storage.append(opaqueBuffer, timed(timestampId, "2023-11-14T22:13:20.900Z"));
        assert(storage.history(opaqueBuffer, 1, timestampId).front().id == "middle");
        storage.clearHistory();

        auto oldNick = sendBuffer("Libera/FractionOld");
        auto newNick = sendBuffer("Libera/FractionNew");
        oldNick.readMarker = "read-late";
        newNick.readMarker = "read-early";
        oldNick.unread = newNick.unread = 5;
        model.buffers[oldNick.id] = oldNick;
        model.buffers[newNick.id] = newNick;
        storage.save(model);
        storage.append(oldNick.id, timed("read-late", "2023-11-14T22:13:20.900Z"));
        storage.append(oldNick.id, timed("unread", "2023-11-14T22:13:20.950Z"));
        storage.append(newNick.id, timed("middle", "2023-11-14T22:13:20.500Z"));
        storage.append(newNick.id, timed("read-early", "2023-11-14T22:13:20.100Z"));
        storage.renameBuffer(oldNick.id, newNick.id);
        storage.load(model);
        assert(!model.buffers.contains(oldNick.id));
        assert(model.buffers.at(newNick.id).readMarker == "read-late" &&
               model.buffers.at(newNick.id).unread == 1);
        // Timestamp markers include their exact fractional instant, not the entire second.
        storage.read(newNick.id, "2023-11-14T23:13:20.800+01:00");
        storage.read(oldNick.id, "2023-11-14T22:13:20.200Z");
        storage.renameBuffer(newNick.id, oldNick.id);
        storage.load(model);
        assert(model.buffers.at(oldNick.id).readMarker == "2023-11-14T23:13:20.800+01:00" &&
               model.buffers.at(oldNick.id).unread == 2);
    }
    {
        Storage storage;
        Model model;
        storage.load(model);
        const auto &renamed = model.buffers.at("Libera/FractionOld");
        assert(renamed.readMarker == "2023-11-14T23:13:20.800+01:00" && renamed.unread == 2);
        storage.clearHistory();
        Settings settings = model.settings;
        settings.maxMessages = 2;
        storage.prune(settings);
        storage.append(id, timed("late", "2023-11-14T22:13:20.900Z"));
        storage.append(id, timed("early", "2023-11-14T22:13:20.100Z"));
        storage.append(id, timed("middle", "2023-11-14T22:13:20.500Z"));
        const auto retained = storage.history(id);
        assert(retained.size() == 2 && retained[0].id == "middle" && retained[1].id == "late");
        storage.clearHistory();
    }
    {
        Storage storage;
        Model model;
        storage.load(model);
        model.settings.retentionDays = 0;
        model.settings.maxMessages = 1000;
        storage.save(model);
        storage.append(id, timed("legacy-late", "2023-11-14T22:13:20.900Z"));
        storage.append(id, timed("legacy-offset", "2023-11-15T00:13:20.100+02:00"));
        storage.append(id, timed("legacy-invalid", "2023-11-14T22:13:20.900Zjunk"));
    }
    // Restore an actual v4 index layout without rewriting any legacy message values.
    dropHistorySchema(database);
    sql(database, "DROP INDEX messages_buffer_order; DROP INDEX messages_order; "
                  "DROP INDEX messages_provisional; DROP INDEX messages_time; "
                  "ALTER TABLE messages DROP COLUMN activity; "
                  "ALTER TABLE messages DROP COLUMN activity_target; "
                  "ALTER TABLE buffers DROP COLUMN muted; "
                  "ALTER TABLE buffers DROP COLUMN archived; "
                  "CREATE INDEX messages_buffer_order ON messages(buffer,sort_time,seq); "
                  "CREATE INDEX messages_order ON messages(sort_time,seq); PRAGMA user_version=4;");
    for (int reopen = 0; reopen < 2; ++reopen) {
        Storage migrated;
        const auto rows = migrated.history(id);
        assert(rows.size() == 3 && rows[0].id == "legacy-invalid" &&
               rows[1].id == "legacy-offset" && rows[2].id == "legacy-late");
        assert(rows[0].timestamp == second && rows[0].time == "2023-11-14T22:13:20.900Zjunk" &&
               rows[1].timestamp == second && rows[1].time == "2023-11-15T00:13:20.100+02:00");
    }
    // Upgrade a v5 database too, then preserve raw-time boundaries with inconsistent
    // legacy seconds instead of replacing them with parsed timestamp seconds.
    dropHistorySchema(database);
    sql(database, "DROP INDEX messages_time; ALTER TABLE messages DROP COLUMN activity; "
                  "ALTER TABLE messages DROP COLUMN activity_target; "
                  "ALTER TABLE buffers DROP COLUMN muted; "
                  "ALTER TABLE buffers DROP COLUMN archived; "
                  "PRAGMA user_version=5;");
    const string rawTime = "2023-11-14T22:13:20.500Z";
    const string opaqueTimeId = "2023-11-14T22:13:20.700Z";
    string localCursor;
    {
        Storage storage;
        Settings settings;
        settings.retentionDays = 0;
        settings.maxMessages = 1000;
        storage.prune(settings);
        storage.clearHistory();
        storage.append(id, message("raw-older", second - 20));
        auto raw = timed("raw-earliest", rawTime);
        raw.timestamp = second - 10;
        storage.append(id, raw);
        raw.id = "raw-later";
        raw.timestamp = second - 5;
        storage.append(id, raw);
        storage.append(id, timed("canonical-peer", rawTime));
        storage.append(id, timed("canonical-after", "2023-11-14T22:13:20.600Z"));
        storage.append(id, timed("opaque-time-match", opaqueTimeId));
        storage.append(id, timed(opaqueTimeId, "2023-11-14T22:13:20.900Z"));
        storage.append(id, message("", second + 10));
        const auto seeded = storage.history(id);
        assert(seeded.size() == 8);
        localCursor = seeded.back().id;
        assert(localCursor.starts_with("local:"));
        auto collision = timed("local-time-match", localCursor);
        collision.timestamp = second - 15;
        storage.append(id, collision);
    }
    for (int reopen = 0; reopen < 2; ++reopen) {
        Storage storage;
        assert(storage.history(id, 1, rawTime).front().id == "local-time-match");
        // A new UTC-equivalent spelling is an instant boundary, excluding its whole tie.
        assert(storage.history(id, 1, "2023-11-15T00:13:20.500+02:00").front().id == "raw-later");
        assert(storage.history(id, 1, opaqueTimeId).front().id == "opaque-time-match");
        assert(storage.history(id, 1, localCursor).front().id == "raw-older");
        assert(storage.message(id, "raw-earliest")->timestamp == second - 10 &&
               storage.message(id, "raw-earliest")->time == rawTime);
        bool rejected = false;
        try {
            storage.history(id, 1, "local:0" + localCursor.substr(6));
        } catch (const std::runtime_error &) {
            rejected = true;
        }
        assert(rejected);
    }
    {
        Storage storage;
        storage.clearHistory();
    }
    sqlite3_reset_auto_extension();
    assert(scalar(database, "PRAGMA user_version") == schema);
}
static void queuedSends(const fs::path &database, int64_t now) {
    const string id = "Libera/#queued", first = "queued_%'first";
    const string original = "  first\noriginal \n";
    const string second("second\0backup", sizeof("second\0backup") - 1);
    const string newer = "\nnewer draft  ";
    const string recovered = original + "\n" + second + "\n" + newer;
    auto ghost = [&](string messageId, string label) {
        auto value = message(std::move(messageId), now, "partial ghost");
        value.own = value.pending = true;
        value.label = std::move(label);
        value.reactions["👍"] = {"alice"};
        return value;
    };
    {
        Storage storage;
        Model model;
        storage.load(model);
        model.settings.retentionDays = 0;
        model.settings.maxMessages = 100;
        Buffer buffer;
        buffer.id = id;
        buffer.server = "Libera";
        buffer.target = "#queued";
        buffer.title = "Queued buffer";
        buffer.draft = newer;
        model.buffers.emplace(id, buffer);
        storage.save(model);
        storage.queueSend(buffer, first, "replaced backup");
        storage.queueSend(buffer, "queued-second", second);
        storage.queueSend(buffer, first, original); // An upsert must retain enqueue order.
        storage.save(model); // Saving a newer visible draft cannot retire its queued backups.
        assert(model.buffers.at(id).draft == newer && storage.history(id).empty());
        assert(scalar(database, "SELECT COUNT(*) FROM queued_sends") == 2);
        storage.append(id, ghost("partial-first", first));
        auto part = ghost("partial-descendant", first + ".part1");
        storage.append(id, part);
        part.pending = false;
        storage.update(id, part); // Auto-solidification is still a provisional local echo.
        storage.append(id, ghost("unrelated-prefix", first + "-other"));
        auto incoming = ghost("incoming-same-label", first + ".part2");
        incoming.own = false;
        storage.append(id, incoming);
        auto confirmed = ghost("confirmed-same-label", first + ".part3");
        confirmed.pending = false;
        storage.append(id, confirmed, "", 0, true);
        const auto rows = storage.history(id, -1);
        assert(rows.size() == 5 && rows[1].id == part.id && !rows[1].pending &&
               rows[1].sendStatus == SendStatus::Unknown);
        assert(rows[3].id == incoming.id && rows[3].sendStatus == SendStatus::None);
        assert(rows[4].id == confirmed.id && rows[4].sendStatus == SendStatus::Confirmed);
        assert(storage.message(id, part.id)->sendStatus == SendStatus::Unknown);
    } // Crash before the last part: both backups, not partial ghosts, own recovery.
    {
        Storage storage;
        Model model;
        storage.load(model);
        const auto &buffer = model.buffers.at(id);
        assert(buffer.draft == recovered && buffer.title == "Queued buffer");
        assert(buffer.messages.size() == 3);
        assert(buffer.messages[0].id == "unrelated-prefix" &&
               buffer.messages[0].sendStatus == SendStatus::Unknown);
        assert(buffer.messages[1].id == "incoming-same-label" &&
               buffer.messages[1].sendStatus == SendStatus::None);
        assert(buffer.messages[2].id == "confirmed-same-label" &&
               buffer.messages[2].sendStatus == SendStatus::Confirmed);
        const auto rows = storage.history(id, -1);
        assert(rows.size() == 3 && rows[0].sendStatus == SendStatus::Unknown &&
               rows[1].sendStatus == SendStatus::None &&
               rows[2].sendStatus == SendStatus::Confirmed);
        assert(!storage.message(id, "partial-first") && !storage.message(id, "partial-descendant"));
        assert(storage.message(id, "unrelated-prefix") &&
               storage.message(id, "incoming-same-label") &&
               storage.message(id, "confirmed-same-label"));
        assert(scalar(database, "SELECT COUNT(*) FROM queued_sends") == 0);
        assert(scalar(database, "SELECT COUNT(*) FROM reactions WHERE message NOT IN "
                                "(SELECT seq FROM messages)") == 0);
        storage.queueSend(buffer, "runtime-rejection", "rejected\noriginal");
        storage.append(id, ghost("runtime-part", "runtime-rejection.part1"));
        auto &restored = model.buffers.at(id);
        restored.draft = "rejected\noriginal\n" + restored.draft;
        restored.title = "Recovered metadata";
        storage.recoverSend(restored, "runtime-rejection");
        assert(!storage.message(id, "runtime-part"));
    }
    {
        Storage storage;
        Model model;
        storage.load(model); // Neither startup nor explicit recovery needs a later save.
        auto &buffer = model.buffers.at(id);
        assert(buffer.draft == "rejected\noriginal\n" + recovered &&
               buffer.title == "Recovered metadata");
        buffer.draft = "newer visible";
        storage.save(model);
        storage.clearHistory(id);
        storage.queueSend(buffer, "completed-append", "sent append");
        storage.append(id, ghost("completed-first", "completed-append"));
        assert(storage.append(id, ghost("completed-last", "completed-append.part1"),
                              "completed-append", 2));
        storage.queueSend(buffer, "completed-update", "sent update");
        auto last = ghost("completed-update-row", "completed-update");
        storage.append(id, last);
        assert(storage.update(id, last, "completed-update", 1));
        assert(scalar(database, "SELECT COUNT(*) FROM queued_sends") == 0);
    }
    {
        Storage storage;
        Model model;
        storage.load(model);
        assert(model.buffers.at(id).draft == "newer visible" &&
               model.buffers.at(id).messages.size() == 3);
        storage.queueSend(model.buffers.at(id), "atomic-append", "append original");
        storage.queueSend(model.buffers.at(id), "atomic-update", "update original");
        storage.queueSend(model.buffers.at(id), "atomic-recover", "recover original");
        storage.append(id, ghost("atomic-append-first", "atomic-append"));
        storage.append(id, ghost("atomic-append-middle", "atomic-append.part1"));
        auto existing = ghost("atomic-update-row", "atomic-update");
        storage.append(id, existing);
        storage.append(id, ghost("atomic-recover-row", "atomic-recover"));
        sql(database, "CREATE TRIGGER fail_queue_retirement BEFORE DELETE ON queued_sends "
                      "BEGIN SELECT RAISE(ABORT,'injected queue retirement failure'); END;");
        bool failed = false;
        try {
            storage.append(id, ghost("atomic-final", "atomic-append.part2"), "atomic-append", 3);
        } catch (const std::runtime_error &) {
            failed = true;
        }
        assert(failed && !storage.message(id, "atomic-final"));
        existing.text = "changed final";
        failed = false;
        try {
            storage.update(id, existing, "atomic-update", 1);
        } catch (const std::runtime_error &) {
            failed = true;
        }
        assert(failed && storage.message(id, existing.id)->text == "partial ghost");
        auto restored = model.buffers.at(id);
        restored.draft = "recover original\n" + restored.draft;
        failed = false;
        try {
            storage.recoverSend(restored, "atomic-recover");
        } catch (const std::runtime_error &) {
            failed = true;
        }
        assert(failed && storage.message(id, "atomic-recover-row"));
        assert(scalar(database, "SELECT COUNT(*) FROM buffers WHERE id='Libera/#queued' "
                                "AND draft='newer visible'") == 1);
        assert(scalar(database, "SELECT COUNT(*) FROM queued_sends") == 3);
        sql(database, "DROP TRIGGER fail_queue_retirement;");
    }
    {
        Storage storage;
        Model model;
        storage.load(model);
        assert(model.buffers.at(id).draft ==
               "append original\nupdate original\nrecover original\nnewer visible");
        assert(model.buffers.at(id).messages.size() == 3);
        storage.load(model);
        assert(model.buffers.at(id).draft ==
               "append original\nupdate original\nrecover original\nnewer visible");
        storage.queueSend(sendBuffer("Libera/QueueOnly"), "queue-only", "no saved buffer");
    }
    {
        Storage storage;
        Model model;
        storage.load(model);
        const auto &buffer = model.buffers.at("Libera/QueueOnly");
        assert(buffer.draft == "no saved buffer" && buffer.server == "Libera" &&
               buffer.target == "QueueOnly" && buffer.title == "QueueOnly");
        storage.queueSend(buffer, "closed-queue", "intentionally discarded");
        model.buffers.erase(buffer.id);
        storage.save(model);
        assert(scalar(database, "SELECT COUNT(*) FROM queued_sends") == 0);
        storage.queueSend(sendBuffer("Libera/QueueOnly"), "reopened-send", "reopened before save");
    }
    {
        Storage storage;
        Model model;
        storage.load(model);
        assert(model.buffers.at("Libera/QueueOnly").draft == "reopened before save");
        storage.append("Libera/QueueOnly", message("closed-history", now, "preserved history"));
        storage.queueSend(model.buffers.at("Libera/QueueOnly"), "close-one", "discard one");
        storage.queueSend(model.buffers.at("Libera/QueueOnly"), "close-two", "discard two");
        storage.append("Libera/QueueOnly", ghost("closed-pending", "close-one"));
        storage.append("Libera/QueueOnly", ghost("closed-descendant", "close-two.part1"));
        storage.queueSend(sendBuffer("Libera/#close-unrelated"), "close-unrelated",
                          "preserve unrelated");
        storage.append("Libera/#close-unrelated", ghost("unrelated-pending", "close-unrelated"));
        storage.closeBuffer(model.buffers.at("Libera/QueueOnly")); // Close before any model save.
        assert(!storage.message("Libera/QueueOnly", "closed-pending") &&
               !storage.message("Libera/QueueOnly", "closed-descendant"));
        assert(storage.message("Libera/QueueOnly", "closed-history") &&
               storage.message("Libera/#close-unrelated", "unrelated-pending"));
        assert(scalar(database, "SELECT COUNT(*) FROM queued_sends") == 1);
        storage.load(model);
        assert(!model.buffers.contains("Libera/QueueOnly") &&
               model.buffers.at("Libera/#close-unrelated").draft == "preserve unrelated");
        storage.queueSend(sendBuffer("Libera/OldQueued"), "renamed-queue", "renamed backup");
        storage.renameBuffer("Libera/OldQueued", "Libera/NewQueued");
        storage.queueSend(sendBuffer("Libera/#unrelated-queue"), "unrelated-queue",
                          "keep this backup");
        storage.clearHistory("Libera/NewQueued");
        assert(scalar(database, "SELECT COUNT(*) FROM queued_sends WHERE "
                                "buffer='Libera/#unrelated-queue'") == 1);
        assert(scalar(database, "SELECT COUNT(*) FROM queued_sends WHERE "
                                "buffer='Libera/NewQueued'") == 1);
        assert(scalar(database, "SELECT COUNT(*) FROM queued_sends WHERE "
                                "buffer='Libera/OldQueued'") == 0);
        storage.clearHistory();
        assert(scalar(database, "SELECT COUNT(*) FROM queued_sends") == 2);
        storage.load(model);
        assert(model.buffers.at("Libera/NewQueued").draft == "renamed backup" &&
               model.buffers.at("Libera/#unrelated-queue").draft == "keep this backup");
        assert(scalar(database, "SELECT COUNT(*) FROM queued_sends") == 0);
    }
    {
        Storage storage;
        const string retry = "Libera/#multiline-retry", submission = "multiline-to-legacy";
        storage.queueSend(sendBuffer(retry), submission, original);
        auto full = ghost("retry-first", submission);
        full.text = original;
        assert(storage.append(retry, full, submission, 1)); // Full multiline ghost is durable.
        assert(scalar(database, "SELECT COUNT(*) FROM queued_sends") == 0);
        storage.queueSend(sendBuffer(retry), submission, original); // Rebackup before overwrite.
        full.text = "  first";
        storage.update(retry, full);
        storage.append(retry, ghost("retry-second", submission + ".part1"));
    } // Crash midway through the multiline-to-legacy retry.
    {
        Storage storage;
        Model model;
        storage.load(model);
        assert(model.buffers.at("Libera/#multiline-retry").draft == original &&
               model.buffers.at("Libera/#multiline-retry").messages.empty());
        storage.load(model);
        assert(model.buffers.at("Libera/#multiline-retry").draft == original);
        assert(scalar(database, "SELECT COUNT(*) FROM queued_sends") == 0);
    }
    dropHistorySchema(database);
    sql(database, "DROP TABLE queued_sends; ALTER TABLE messages DROP COLUMN activity; "
                  "ALTER TABLE messages DROP COLUMN activity_target; "
                  "ALTER TABLE buffers DROP COLUMN muted; "
                  "ALTER TABLE buffers DROP COLUMN archived; "
                  "PRAGMA user_version=3;");
    {
        Storage migrated;
        Model model;
        migrated.load(model);
        assert(model.buffers.at(id).draft ==
               "append original\nupdate original\nrecover original\nnewer visible");
        migrated.queueSend(model.buffers.at(id), "migrated-queued-send", "upgrade backup");
        migrated.load(model);
        assert(model.buffers.at(id).draft ==
               "upgrade backup\nappend original\nupdate original\nrecover original\nnewer visible");
        assert(!model.buffers.contains("Libera/QueueOnly"));
    }
}
static void sendBoundaries(const fs::path &database, int64_t now) {
    const string id = "Libera/#send-boundary";
    auto ghost = [&](const string &label, const string &text) {
        auto value = message("local:boundary:" + label, now, text);
        value.own = value.pending = true;
        value.label = label;
        return value;
    };
    auto partLabel = [](const string &root, size_t part) {
        return part ? root + ".part" + std::to_string(part) : root;
    };
    // Identical text is not evidence that an editable draft is the submitted draft.
    for (const string &saved : {string("X"), string("older partial")})
        for (const string &newer : {string{}, string("X")}) {
            {
                Storage storage;
                Model model;
                storage.load(model);
                storage.clearHistory();
                model.settings.retentionDays = 0;
                model.settings.maxMessages = 200;
                auto buffer = sendBuffer(id);
                buffer.draft = saved;
                model.buffers[id] = buffer;
                storage.save(model);
                buffer.draft = newer;
                storage.queueSend(buffer, "draft-boundary", "X");
            } // No save after submission: queueSend owns this exact crash boundary.
            Storage restarted;
            Model recovered;
            restarted.load(recovered);
            assert(recovered.buffers.at(id).draft == (newer.empty() ? "X" : "X\nX"));
            restarted.load(recovered);
            assert(recovered.buffers.at(id).draft == (newer.empty() ? "X" : "X\nX"));
        }
    {
        Storage storage;
        Model model;
        storage.load(model);
        auto &buffer = model.buffers.at(id);
        buffer.draft = "saved draft";
        storage.save(model);
        storage.closeBuffer(buffer);
        buffer.draft.clear();
        sql(database, "CREATE TRIGGER fail_queue_insert BEFORE INSERT ON queued_sends "
                      "BEGIN SELECT RAISE(ABORT,'injected queue insertion failure'); END;");
        bool failed = false;
        try {
            storage.queueSend(buffer, "atomic-queue", "X");
        } catch (const std::runtime_error &) {
            failed = true;
        }
        assert(failed && scalar(database, "SELECT COUNT(*) FROM buffers WHERE "
                                          "id='Libera/#send-boundary' AND draft='' "
                                          "AND server='Libera' AND target='#send-boundary'") == 1);
        assert(scalar(database, "SELECT COUNT(*) FROM closed_buffers WHERE "
                                "id='Libera/#send-boundary'") == 1);
        assert(scalar(database, "SELECT COUNT(*) FROM queued_sends") == 0);
        sql(database, "DROP TRIGGER fail_queue_insert;");
    }
    string original;
    for (size_t part = 0; part < 201; ++part) {
        if (part)
            original += '\n';
        original += "line " + std::to_string(part);
    }
    {
        Storage storage;
        Model model;
        storage.load(model);
        storage.queueSend(sendBuffer(id), "over-cap", original);
        for (size_t part = 0; part < 201; ++part) {
            auto row = ghost(partLabel("over-cap", part), "line " + std::to_string(part));
            assert(!storage.append(id, row, part == 200 ? "over-cap" : "", 201));
        }
        assert(storage.history(id).empty() && storage.history(id, -1).size() == 200);
        assert(scalar(database, "SELECT COUNT(*) FROM queued_sends") == 1);
    }
    {
        Storage storage;
        Model model;
        storage.load(model);
        assert(model.buffers.at(id).draft == original && model.buffers.at(id).messages.empty());
        storage.load(model);
        assert(model.buffers.at(id).draft == original);
        model.buffers.at(id).draft.clear();
        model.settings.maxMessages = 1000;
        storage.save(model);
        storage.queueSend(model.buffers.at(id), "beyond-tail", original);
        for (size_t part = 0; part < 201; ++part) {
            auto row = ghost(partLabel("beyond-tail", part), "line " + std::to_string(part));
            const bool retired = storage.append(id, row, part == 200 ? "beyond-tail" : "", 201);
            assert(retired == (part == 200));
            row.pending = false;
            storage.update(id, row); // A visual timeout is not actual confirmation.
        }
        assert(storage.history(id).empty() && storage.history(id, -1).size() == 201);
        for (const auto &row : storage.history(id, -1))
            assert(row.own && !row.pending && row.sendStatus == SendStatus::Unknown);
        assert(scalar(database, "SELECT COUNT(*) FROM messages WHERE buffer="
                                "'Libera/#send-boundary' AND pending<>0") == 0);
        assert(scalar(database, "SELECT COUNT(*) FROM queued_sends") == 0);
    }
    {
        Storage storage;
        Model model;
        storage.load(model);
        const auto &buffer = model.buffers.at(id);
        assert(buffer.draft.empty() && buffer.messages.size() == 201);
        for (size_t part = 0; part < buffer.messages.size(); ++part) {
            const auto &row = buffer.messages[part];
            assert(row.own && row.pending && row.sendStatus == SendStatus::Unknown &&
                   row.label == partLabel("beyond-tail", part) &&
                   row.text == "line " + std::to_string(part));
        }
        for (const auto &row : storage.history(id, -1))
            assert(row.own && !row.pending && row.sendStatus == SendStatus::Unknown);
        storage.clearHistory(id);
        storage.queueSend(buffer, "cleared-handoff", "first\nlast");
        storage.append(id, ghost("cleared-handoff", "first"));
        storage.clearHistory(id);
        assert(!storage.append(id, ghost("cleared-handoff.part1", "last"), "cleared-handoff", 2));
        assert(scalar(database, "SELECT COUNT(*) FROM queued_sends") == 1);
    }
    {
        Storage storage;
        Model model;
        storage.load(model);
        auto &buffer = model.buffers.at(id);
        assert(buffer.draft == "first\nlast" && buffer.messages.empty());
        buffer.draft.clear();
        storage.queueSend(buffer, "exact_%'namespace", "first\nlast");
        storage.append(id, ghost("exact_%'namespace", "first"));
        storage.append(id, ghost("exact_%'namespace.part01", "not part one"));
        storage.append(id, ghost("exact_%'namespace.part1-other", "not a part"));
        auto incoming = ghost("exact_%'namespace.part1", "incoming");
        incoming.own = false;
        storage.append(id, incoming);
        assert(!storage.update(id, ghost("exact_%'namespace", "first"), "exact_%'namespace", 2));
        assert(
            storage.append(id, ghost("exact_%'namespace.part1", "last"), "exact_%'namespace", 2));
        storage.clearHistory();
        model.settings.maxMessages = 200;
        storage.save(model);
        storage.queueSend(buffer, "confirmed-over-cap", original);
        for (size_t part = 0; part < 201; ++part)
            assert(!storage.append(
                id, ghost(partLabel("confirmed-over-cap", part), "line " + std::to_string(part)),
                part == 200 ? "confirmed-over-cap" : "", 201));
        for (size_t part = 0; part < 201; ++part) {
            auto row = ghost(partLabel("confirmed-over-cap", part), "line " + std::to_string(part));
            row.pending = false;
            row.timestamp = 0; // Actual echoes may add neither an ID nor server time.
            const bool retired =
                storage.update(id, row, part == 200 ? "confirmed-over-cap" : "", 201, true);
            assert(retired == (part == 200));
        }
        assert(scalar(database, "SELECT COUNT(*) FROM queued_sends") == 0);
        assert(scalar(database, "SELECT COUNT(*) FROM messages WHERE own=1 AND local_echo=1") == 0);
    }
    {
        Storage storage;
        Model model;
        storage.load(model);
        assert(model.buffers.at(id).draft.empty() && model.buffers.at(id).messages.size() == 200);
        for (const auto &row : model.buffers.at(id).messages)
            assert(row.own && !row.pending && row.sendStatus == SendStatus::Confirmed);
        storage.clearHistory();
        auto buffer = sendBuffer(id);
        buffer.draft = "genuinely newer";
        storage.queueSend(buffer, "confirmed-retry", "already sent");
        auto row = ghost("confirmed-retry", "already sent");
        storage.append(id, row);
        row.pending = false;
        row.timestamp = 0;
        storage.update(id, row, "", 0, true);
        sql(database, "CREATE TRIGGER fail_confirmed_retirement BEFORE DELETE ON queued_sends "
                      "BEGIN SELECT RAISE(ABORT,'injected confirmed retirement failure'); END;");
        bool failed = false;
        try {
            storage.update(id, row, "confirmed-retry", 1, true);
        } catch (const std::runtime_error &) {
            failed = true;
        }
        assert(failed && scalar(database, "SELECT COUNT(*) FROM queued_sends") == 1);
        sql(database, "DROP TRIGGER fail_confirmed_retirement;");
        storage.recoverSend(buffer, "confirmed-retry");
        storage.load(model);
        assert(model.buffers.at(id).draft == "genuinely newer" &&
               model.buffers.at(id).messages.size() == 1 &&
               !model.buffers.at(id).messages.front().pending &&
               model.buffers.at(id).messages.front().sendStatus == SendStatus::Confirmed);
        assert(storage.message(id, row.id)->sendStatus == SendStatus::Confirmed);
        auto presence = message("own-presence", 0);
        presence.own = presence.presence = true;
        storage.append(id, presence);
        assert(storage.history(id, -1).back().sendStatus == SendStatus::None);
        storage.load(model);
        assert(model.buffers.at(id).messages.size() == 1 &&
               model.buffers.at(id).messages.front().sendStatus == SendStatus::Confirmed);
        storage.clearHistory();
    }
    const string closed = "Libera/#handed-close", savedClose = "Libera/#saved-close";
    {
        Storage storage;
        Model model;
        storage.load(model);
        for (const auto &target : {closed, savedClose}) {
            auto buffer = sendBuffer(target);
            model.buffers[target] = buffer;
            storage.queueSend(buffer, "handed-off", "discard unconfirmed");
            auto pending = ghost("handed-off", "discard unconfirmed");
            assert(storage.append(target, pending, "handed-off", 1));
            pending.pending = false;
            storage.update(target, pending);
            auto confirmed = ghost("actual-confirmed", "preserve confirmed");
            storage.append(target, confirmed);
            confirmed.pending = false;
            confirmed.timestamp = 0;
            storage.update(target, confirmed, "", 0, true);
            storage.append(target, message("ordinary-history", now, "preserve incoming"));
            storage.append(target, ghost("orphan-provisional.part1", "discard without backup"));
        }
        const string unrelated = "Libera/#close-boundary-unrelated";
        storage.append(unrelated, ghost("handed-off", "preserve unrelated"));
        assert(scalar(database, "SELECT COUNT(*) FROM queued_sends") == 0);
        storage.closeBuffer(model.buffers.at(closed), {"handed-off", "actual-confirmed"});
        assert(!storage.message(closed, "local:boundary:handed-off") &&
               !storage.message(closed, "local:boundary:orphan-provisional.part1"));
        assert(storage.message(closed, "local:boundary:actual-confirmed") &&
               storage.message(closed, "ordinary-history") &&
               storage.message(unrelated, "local:boundary:handed-off"));
        storage.load(model);
        assert(!model.buffers.contains(closed));
        model.buffers.erase(savedClose);
        storage.save(model); // Save-detected closes use the same independent discovery.
        assert(!storage.message(savedClose, "local:boundary:handed-off") &&
               !storage.message(savedClose, "local:boundary:orphan-provisional.part1"));
        assert(storage.message(savedClose, "local:boundary:actual-confirmed") &&
               storage.message(savedClose, "ordinary-history") &&
               storage.message(unrelated, "local:boundary:handed-off"));
        storage.load(model);
        assert(!model.buffers.contains(closed) && !model.buffers.contains(savedClose));
    }
}
static void spellingPreferences(const fs::path &config) {
    const auto preferences = config / "settings.toml";
    // Old settings keep the new defaults; safe unavailable IDs survive a restart.
    for (const string language : {"", "auto", "en_US", "missing_dictionary-42"}) {
        {
            std::ofstream output(preferences);
            output << "schema_version = 1\n";
            if (!language.empty())
                output << "[composer]\nspellcheck = false\nspellcheck_language = '" << language
                       << "'\n";
        }
        for (int reopen = 0; reopen < 2; ++reopen) {
            Storage storage;
            Model model;
            storage.load(model);
            assert(model.settings.spellcheck == language.empty());
            assert(model.settings.spellcheckLanguage == (language.empty() ? "auto" : language));
            storage.save(model);
        }
    }
    const auto valid = contents(preferences);
    for (const string language : {"", "../en_US", "/en_US", "en/US", "en\\US", ".hidden",
                                  "en_US.dic", "en US", "_en", "-en", "é"}) {
        Storage storage;
        Model model;
        storage.load(model);
        model.settings.spellcheckLanguage = language;
        bool failed = false;
        try {
            storage.save(model);
        } catch (const std::runtime_error &) {
            failed = true;
        }
        assert(failed && contents(preferences) == valid);
    }
    for (const string source :
         {"spellcheck = 'true'\n", "spellcheck_language = 12\n",
          "spellcheck_language = '../en_US'\n", "spellcheck_language = ''\n"}) {
        {
            std::ofstream output(preferences);
            output << "schema_version = 1\n[composer]\n" << source;
        }
        Storage storage;
        Model model;
        model.settings.spellcheckLanguage = "unchanged";
        bool failed = false;
        try {
            storage.load(model);
        } catch (const std::runtime_error &) {
            failed = true;
        }
        assert(failed && model.settings.spellcheckLanguage == "unchanged");
    }
    std::ofstream(preferences) << valid;
}
static void predictionPreferences(const fs::path &config) {
    const auto preferences = config / "settings.toml";
    const auto original = contents(preferences);
    for (const string source : {"", "[composer]\n", "[composer]\nword_prediction = false\n",
                                "[composer]\nword_prediction = true\n"}) {
        std::ofstream(preferences) << "schema_version = 1\n" << source;
        const bool enabled = source.find("true") != string::npos;
        for (int reopen = 0; reopen < 2; ++reopen) {
            Storage storage;
            Model model;
            assert(!model.settings.wordPrediction);
            storage.load(model);
            assert(model.settings.wordPrediction == enabled);
            storage.save(model);
            assert(contents(preferences)
                       .find(enabled ? "word_prediction = true" : "word_prediction = false") !=
                   string::npos);
        }
    }
    {
        Storage storage;
        Model model;
        storage.load(model);
        assert(model.settings.wordPrediction);
        model.settings.wordPrediction = Settings{}.wordPrediction;
        storage.save(model);
        model.settings.wordPrediction = true;
        storage.load(model);
        assert(!model.settings.wordPrediction);
    }
    for (const string value : {"'true'", "'false'", "0", "1", "1.0", "[]", "{}"}) {
        std::ofstream(preferences)
            << "schema_version = 1\n[composer]\nword_prediction = " << value << '\n';
        Storage storage;
        Model model;
        model.settings.wordPrediction = true;
        bool failed = false;
        try {
            storage.load(model);
        } catch (const std::runtime_error &) {
            failed = true;
        }
        assert(failed && model.settings.wordPrediction);
    }
    std::ofstream(preferences) << original;
}
static void peopleModePreferences(const fs::path &config) {
    struct Case {
        string source;
        SidebarMode expected;
    };
    const Case cases[] = {
        {"people_mode = 'expanded'\n", SidebarMode::Expanded},
        {"people_mode = 'compact'\n", SidebarMode::Compact},
        {"people_mode = 'hidden'\n", SidebarMode::Hidden},
        {"people_visible = true\n", SidebarMode::Expanded},
        {"people_visible = false\n", SidebarMode::Hidden},
        {"people_mode = 'hidden'\npeople_visible = true\n", SidebarMode::Hidden},
        {"people_mode = 'compact'\npeople_visible = true\n", SidebarMode::Compact},
        {"people_mode = 'expanded'\npeople_visible = false\n", SidebarMode::Expanded},
        {"people_mode = 'compact'\npeople_visible = 'malformed legacy'\n", SidebarMode::Compact}};
    for (const auto &test : cases) {
        {
            std::ofstream output(config / "settings.toml");
            output << "schema_version = 1\n[workspace]\nfuture_layout = 'preserved'\n"
                   << test.source;
        }
        for (int reopen = 0; reopen < 2; ++reopen) {
            Storage storage;
            Model model;
            storage.load(model);
            assert(model.settings.peopleMode == test.expected &&
                   model.settings.workspace.at("future_layout") == "preserved" &&
                   !model.settings.workspace.contains("people_visible") &&
                   !model.settings.workspace.contains("people_mode"));
            if (reopen == 0) {
                // Reserved legacy entries cannot leak through the arbitrary workspace map.
                model.settings.workspace["people_visible"] = "obsolete";
                model.settings.workspace["people_mode"] = "obsolete";
                storage.save(model);
                const string saved = contents(config / "settings.toml");
                assert(saved.find("people_mode") != string::npos &&
                       saved.find("people_visible") == string::npos);
            }
        }
    }
}
static void presenceInheritancePreferences(const fs::path &config) {
    {
        std::ofstream output(config / "settings.toml");
        output << "schema_version = 1\n[presence]\nmode = 'smart'\n[presence.channels]\n"
                  "'#chan' = 'hidden'\n'networkA/#chan' = 'global'\n"
                  "'networkC/#chan' = 'all'\n'networkD/#chan' = 'smart'\n"
                  "'networkE/#chan' = 'hidden'\n";
    }
    auto check = [](const Settings &settings, PresenceMode global) {
        assert(settings.presence == global &&
               settings.channelPresence.at("networkA/#chan") == PresenceMode::Global);
        assert(effectivePresenceMode(settings, "networkA/#chan", "#chan") == global);
        assert(effectivePresenceMode(settings, "networkB/#chan", "#chan") == PresenceMode::Hidden);
        assert(effectivePresenceMode(settings, "networkC/#chan", "#chan") == PresenceMode::All);
        assert(effectivePresenceMode(settings, "networkD/#chan", "#chan") == PresenceMode::Smart);
        assert(effectivePresenceMode(settings, "networkE/#chan", "#chan") == PresenceMode::Hidden);
        assert(effectivePresenceMode(settings, "networkB/#other", "#other") == global);
    };
    {
        Storage storage;
        Model model;
        storage.load(model);
        check(model.settings, PresenceMode::Smart);
        storage.save(model);
    }
    {
        Storage storage;
        Model model;
        storage.load(model);
        check(model.settings, PresenceMode::Smart);
        model.settings.presence = PresenceMode::All;
        check(model.settings, PresenceMode::All);
        storage.save(model);
    }
    {
        Storage storage;
        Model model;
        storage.load(model);
        check(model.settings, PresenceMode::All);
        const string preferences = contents(config / "settings.toml");
        model.settings.presence = PresenceMode::Global;
        bool failed = false;
        try {
            storage.save(model);
        } catch (const std::runtime_error &error) {
            failed = string(error.what()).find("presence.mode") != string::npos;
        }
        assert(failed && contents(config / "settings.toml") == preferences);
    }
}
static void conversationBindingPreferences(const fs::path &config) {
    {
        std::ofstream output(config / "settings.toml");
        output << "schema_version = 1\n";
    }
    map<string, string> defaults;
    {
        Storage storage;
        Model model;
        storage.load(model);
        defaults = model.settings.bindings;
        assert(defaults.at("conversation-back") == "C-o");
        assert(defaults.at("conversation-forward") == "C-i");
        assert(defaults.at("next-activity") == "C-x C-SPC");
        assert(defaults.at("set-mark") == "C-SPC");
        assert(defaults.at("/join").empty() && defaults.at("mark-read").empty());
    }
    struct Case {
        string source, back, forward;
        map<string, string> retained;
    };
    const Case cases[] = {
        {"", "C-o", "C-i", {}},
        {"toggle-navigation = 'C-o'\n", "", "C-i", {{"toggle-navigation", "C-o"}}},
        {"toggle-navigation = ' c-O '\n", "", "C-i", {{"toggle-navigation", " c-O "}}},
        {"toggle-navigation = ' c-O   X '\n", "", "C-i", {{"toggle-navigation", " c-O   X "}}},
        {"toggle-navigation = 'C-i'\n", "C-o", "", {{"toggle-navigation", "C-i"}}},
        {"toggle-navigation = ' c-I '\n", "C-o", "", {{"toggle-navigation", " c-I "}}},
        {"toggle-navigation = ' c-I   X '\n", "C-o", "", {{"toggle-navigation", " c-I   X "}}},
        {"toggle-navigation = 'C-oops'\n", "C-o", "C-i", {{"toggle-navigation", "C-oops"}}},
        {"conversation-back = ''\nconversation-forward = ''\n", "", "", {}},
        {"conversation-back = 'M-o'\nconversation-forward = 'M-i'\n", "M-o", "M-i", {}},
        {"conversation-back = ' c-O   X '\ntoggle-navigation = 'C-o'\n",
         " c-O   X ",
         "C-i",
         {{"toggle-navigation", "C-o"}}},
        {"conversation-forward = ' c-I   X '\ntoggle-navigation = 'C-i'\n",
         "C-o",
         " c-I   X ",
         {{"toggle-navigation", "C-i"}}},
        {"conversation-back = 'C-i x'\n", "C-i x", "", {}},
        {"conversation-forward = 'C-o x'\n", "", "C-o x", {}},
        {"conversation-back = 'C-i'\nconversation-forward = 'C-o'\n", "C-i", "C-o", {}},
        {"set-mark = ' c-SpC '\n", "C-o", "C-i", {{"set-mark", " c-SpC "}}},
        {"set-mark = 'C-S-SPC'\nnext-activity = 'C-x C-SPC'\n", "C-o", "C-i", {}},
        {"set-mark = 'C-S-SPC'\nnext-activity = 'C-SPC'\n", "C-o", "C-i", {}},
        {"set-mark = 'C-SPC x'\n", "C-o", "C-i", {{"set-mark", "C-SPC x"}}},
        {"set-mark = 'M-SPC'\n", "C-o", "C-i", {{"set-mark", "M-SPC"}}},
        {"set-mark = ''\n", "C-o", "C-i", {{"set-mark", ""}}},
        {"set-mark = 'C-S-SPC'\ntoggle-navigation = ' c-SPC x '\n",
         "C-o",
         "C-i",
         {{"set-mark", "C-S-SPC"}, {"toggle-navigation", " c-SPC x "}}},
        {"next-activity = 'C-S-SPC'\n", "C-o", "C-i", {{"next-activity", "C-S-SPC"}}},
        {"toggle-navigation = 'C-SPC x'\n",
         "C-o",
         "C-i",
         {{"set-mark", ""}, {"toggle-navigation", "C-SPC x"}}},
        {"toggle-navigation = 'C-x'\n",
         "C-o",
         "C-i",
         {{"next-activity", ""},
          {"jump-channel", ""},
          {"switch-buffer", ""},
          {"toggle-fools", ""},
          {"toggle-navigation", "C-x"}}},
        {"next-activity = 'C-SPC'\n", "C-o", "C-i", {}},
        {"set-mark = 'C-SPC'\nnext-activity = 'C-SPC'\n", "C-o", "C-i", {{"set-mark", "C-SPC"}}},
        {"next-activity = 'C-SPC'\ntoggle-navigation = ' c-X  c-SpC k '\n",
         "C-o",
         "C-i",
         {{"next-activity", ""}, {"toggle-navigation", " c-X  c-SpC k "}}},
        {"next-activity = ' c-SpC '\n",
         "C-o",
         "C-i",
         {{"set-mark", ""}, {"next-activity", " c-SpC "}}},
        {"next-activity = 'C-SPC x'\n",
         "C-o",
         "C-i",
         {{"set-mark", ""}, {"next-activity", "C-SPC x"}}},
        {"set-mark = 'C-SPC'\nnext-activity = ''\n",
         "C-o",
         "C-i",
         {{"set-mark", "C-SPC"}, {"next-activity", ""}}},
        {"next-activity = 'M-SPC'\n", "C-o", "C-i", {{"next-activity", "M-SPC"}}},
        {"\"/join\" = 'C-x F8'\nmark-read = 'M-F8'\n",
         "C-o",
         "C-i",
         {{"/join", "C-x F8"}, {"mark-read", "M-F8"}}},
        {"toggle-navigation = ' c-S  X '\nuser-search = 'C-s x'\nempty-command = ''\n",
         "C-o",
         "C-i",
         {{"search", ""},
          {"toggle-navigation", " c-S  X "},
          {"user-search", "C-s x"},
          {"empty-command", ""}}},
        {"\"\" = 'C-x'\nuser-send = ' Enter '\n",
         "C-o",
         "C-i",
         {{"send", ""}, {"user-send", " Enter "}}},
    };
    for (const auto &test : cases) {
        {
            std::ofstream output(config / "settings.toml");
            output << "schema_version = 1\n[appearance]\nopacity = 0.5\n"
                      "[bindings]\nuser-command = 'M-F12'\n"
                   << test.source;
        }
        auto expected = defaults;
        expected.at("conversation-back") = test.back;
        expected.at("conversation-forward") = test.forward;
        expected["user-command"] = "M-F12";
        for (const auto &[name, chord] : test.retained)
            expected[name] = chord;
        for (int reopen = 0; reopen < 2; ++reopen) {
            Storage storage;
            Model model;
            storage.load(model);
            assert(model.settings.bindings == expected);
            assert(model.settings.opacity == .5f);
            if (reopen == 0)
                storage.save(model);
        }
    }
}
static void messageBindingPreferences(const fs::path &config) {
    struct Case {
        string source;
        map<string, string> expected;
    };
    const Case cases[] = {
        {"user-command = 'C-t x'\n", {{"user-command", "C-t x"}, {"transpose-chars", ""}}},
        {"user-command = ' c-T '\n", {{"user-command", " c-T "}, {"transpose-chars", ""}}},
        {"transpose-chars = ''\nbackward-kill-line = 'C-c k'\n",
         {{"transpose-chars", ""}, {"backward-kill-line", "C-c k"}}},
        {"transpose-chars = 'M-t'\nredo = 'C-c r'\ncopy = 'C-c w'\ncut = 'C-c x'\n"
         "paste = 'C-c v'\nselect-all = 'C-c a'\n",
         {{"transpose-chars", "M-t"},
          {"redo", "C-c r"},
          {"copy", "C-c w"},
          {"cut", "C-c x"},
          {"paste", "C-c v"},
          {"select-all", "C-c a"}}},
        {"previous-line = 'C-p'\nnext-line = 'C-n'\nend-of-line = 'C-e'\n",
         {{"previous-line", ""},
          {"next-line", ""},
          {"end-of-line", ""},
          {"message-prev", "C-p"},
          {"message-next", "C-n"},
          {"message-reply", "C-r"},
          {"message-react", "C-e"}}},
        {"previous-line = 'M-p'\nnext-line = 'M-n'\nend-of-line = 'M-e'\n",
         {{"previous-line", "M-p"},
          {"next-line", "M-n"},
          {"end-of-line", "M-e"},
          {"message-prev", "C-p"},
          {"message-next", "C-n"},
          {"message-reply", "C-r"},
          {"message-react", "C-e"}}},
        {"previous-line = 'C-p'\nnext-line = 'C-n'\nend-of-line = 'C-e'\n"
         "message-prev = ''\nmessage-next = ''\nmessage-reply = ''\nmessage-react = ''\n",
         {{"previous-line", "C-p"},
          {"next-line", "C-n"},
          {"end-of-line", "C-e"},
          {"message-prev", ""},
          {"message-next", ""},
          {"message-reply", ""},
          {"message-react", ""}}},
        {"end-of-line = 'C-e'\nmessage-react = 'C-e'\n",
         {{"end-of-line", ""}, {"message-react", "C-e"}}},
        {"end-of-line = 'C-e'\nmessage-react = ''\nmessage-reply = 'C-e a'\n",
         {{"end-of-line", ""}, {"message-react", ""}, {"message-reply", "C-e a"}}},
        {"user-command = 'C-p'\nuser-reply = 'C-r'\n",
         {{"user-command", "C-p"},
          {"user-reply", "C-r"},
          {"message-prev", ""},
          {"message-next", "C-n"},
          {"message-reply", ""},
          {"message-react", "C-e"}}},
        {"user-command = 'C-p x'\nuser-reply = 'C-r x'\n",
         {{"user-command", "C-p x"},
          {"user-reply", "C-r x"},
          {"message-prev", ""},
          {"message-next", "C-n"},
          {"message-reply", ""},
          {"message-react", "C-e"}}},
        {"message-prev = 'C-r'\n",
         {{"message-prev", "C-r"},
          {"message-next", "C-n"},
          {"message-reply", ""},
          {"message-react", "C-e"}}}};
    for (const auto &test : cases) {
        {
            std::ofstream output(config / "settings.toml");
            output << "schema_version = 1\n[bindings]\n" << test.source;
        }
        for (int reopen = 0; reopen < 2; ++reopen) {
            Storage storage;
            Model model;
            storage.load(model);
            for (const auto &[name, binding] : test.expected)
                assert(model.settings.bindings.at(name) == binding);
            if (reopen == 0)
                storage.save(model);
        }
    }
    {
        std::ofstream output(config / "settings.toml");
        output << "schema_version = 1\n[bindings]\nprevious-line = 'C-p'\n";
    }
    {
        Storage storage;
        Model model;
        storage.load(model);
        assert(model.settings.bindings.at("previous-line").empty() &&
               model.settings.bindings.at("message-prev") == "C-p");
        storage.save(model);
    }
    {
        Storage storage;
        Model model;
        storage.load(model);
        model.settings.bindings.at("message-prev").clear();
        model.settings.bindings.at("previous-line") = "C-p";
        storage.save(model);
    }
    {
        Storage storage;
        Model model;
        storage.load(model);
        assert(model.settings.bindings.at("message-prev").empty() &&
               model.settings.bindings.at("previous-line") == "C-p");
    }
}
static void writtenSendStatus(const fs::path &database, int64_t now) {
    const string id = "Libera/#written-send";
    {
        Storage storage;
        Model model;
        storage.load(model);
        model.settings.retentionDays = 0;
        model.settings.maxMessages = 1000;
        model.buffers[id] = sendBuffer(id);
        storage.save(model);
        auto sent = message("written-append", now, "written without acknowledgement");
        sent.own = true;
        sent.sendStatus = SendStatus::Sent;
        storage.append(id, sent);
        const auto appended = storage.message(id, sent.id);
        assert(appended && appended->own && !appended->pending &&
               appended->sendStatus == SendStatus::Sent);
        sent.id = "written-update";
        sent.pending = true;
        sent.sendStatus = SendStatus::Queued;
        storage.append(id, sent);
        sent.pending = false;
        sent.sendStatus = SendStatus::Sent;
        sent.text = "written update without acknowledgement";
        storage.update(id, sent);
        const auto updated = storage.message(id, sent.id);
        assert(updated && updated->own && !updated->pending &&
               updated->sendStatus == SendStatus::Sent && updated->text == sent.text);
        assert(scalar(database, "SELECT COUNT(*) FROM messages WHERE buffer="
                                "'Libera/#written-send' AND local_echo=2 AND pending=0") == 2);
    }
    {
        Storage storage;
        Model model;
        storage.load(model);
        const auto &rows = model.buffers.at(id).messages;
        const auto history = storage.history(id);
        assert(rows.size() == 2 && history.size() == rows.size());
        for (size_t index = 0; index < rows.size(); ++index)
            assert(rows[index].own && !rows[index].pending &&
                   rows[index].sendStatus == SendStatus::Sent &&
                   history[index].id == rows[index].id && !history[index].pending &&
                   history[index].sendStatus == SendStatus::Sent);
        // The ordinary metadata/message autosave path must not reclassify written sends.
        for (auto &row : model.buffers.at(id).messages) {
            row.text += " after autosave";
            storage.update(id, row);
        }
        storage.save(model);
    }
    {
        Storage storage;
        Model model;
        storage.load(model);
        const auto &buffer = model.buffers.at(id);
        assert(buffer.draft.empty() && buffer.messages.size() == 2);
        for (const auto &row : buffer.messages)
            assert(row.own && !row.pending && row.sendStatus == SendStatus::Sent &&
                   row.text.ends_with(" after autosave"));
        assert(scalar(database, "SELECT COUNT(*) FROM queued_sends WHERE "
                                "buffer='Libera/#written-send'") == 0);
    }
    for (SendStatus other : {SendStatus::Queued, SendStatus::Unknown, SendStatus::Confirmed})
        for (bool sentIsSource : {false, true}) {
            const string source = "Libera/WrittenOld", destination = "Libera/WrittenNew";
            const SendStatus expected =
                other == SendStatus::Confirmed ? SendStatus::Confirmed : SendStatus::Sent;
            {
                Storage storage;
                storage.clearHistory(source);
                storage.clearHistory(destination);
                auto sent = message("written-collision", now);
                sent.own = true;
                sent.sendStatus = SendStatus::Sent;
                auto competing = sent;
                competing.pending = other != SendStatus::Confirmed;
                competing.sendStatus = competing.pending ? SendStatus::Queued : other;
                const string competingBuffer = sentIsSource ? destination : source;
                storage.append(sentIsSource ? source : destination, sent);
                storage.append(competingBuffer, competing, "", 0, other == SendStatus::Confirmed);
                if (other == SendStatus::Unknown) {
                    competing.pending = false;
                    competing.sendStatus = SendStatus::Unknown;
                    storage.update(competingBuffer, competing);
                    assert(storage.message(competingBuffer, competing.id)->sendStatus ==
                           SendStatus::Unknown);
                }
                storage.renameBuffer(source, destination);
                const auto merged = storage.message(destination, sent.id);
                assert(merged && merged->own && !merged->pending && merged->sendStatus == expected);
            }
            {
                Storage storage;
                Model model;
                storage.load(model);
                const auto &rows = model.buffers.at(destination).messages;
                assert(rows.size() == 1 && rows[0].own && !rows[0].pending &&
                       rows[0].sendStatus == expected);
                assert(storage.history(source).empty());
            }
        }
    const string acknowledged = "Libera/#written-acknowledged";
    {
        Storage storage;
        auto row = message("local:written-acknowledgement", now, "written before late echo");
        row.own = true;
        row.sendStatus = SendStatus::Sent;
        row.label = "written-acknowledgement-label";
        storage.append(acknowledged, row);
        row.id = "server-written-acknowledgement";
        storage.append(acknowledged, row, "", 0, true);
        assert(!storage.message(acknowledged, "local:written-acknowledgement"));
        const auto confirmed = storage.message(acknowledged, row.id);
        assert(confirmed && confirmed->own && !confirmed->pending &&
               confirmed->sendStatus == SendStatus::Confirmed);
    }
    {
        Storage storage;
        Model model;
        storage.load(model);
        const auto &rows = model.buffers.at(acknowledged).messages;
        const auto history = storage.history(acknowledged);
        assert(rows.size() == 1 && history.size() == 1 &&
               rows[0].id == "server-written-acknowledgement" && rows[0].own && !rows[0].pending &&
               rows[0].sendStatus == SendStatus::Confirmed && history[0].id == rows[0].id &&
               history[0].sendStatus == SendStatus::Confirmed);
    }
}
static void activityRoundTrip(int64_t now) {
    const string id = "Libera/#activity";
    const ActivityKind kinds[] = {ActivityKind::None, ActivityKind::Join, ActivityKind::Part,
                                  ActivityKind::Quit, ActivityKind::Nick};
    {
        Storage storage;
        Model model;
        storage.load(model);
        storage.clearHistory();
        model.settings.retentionDays = 0;
        model.settings.maxMessages = 1000;
        model.buffers[id] = sendBuffer(id);
        storage.save(model);
        for (int index = 0; index < 5; ++index) {
            auto row = message("activity-" + std::to_string(index), now + index);
            row.presence = index != 0;
            row.activity = kinds[index];
            row.activityTarget = index == 4 ? "new-nick" : "";
            storage.append(id, row);
            assert(storage.message(id, row.id)->activity == kinds[index]);
            assert(storage.message(id, row.id)->activityTarget == row.activityTarget);
            row.text = "updated";
            storage.update(id, row);
            assert(storage.message(id, row.id)->activity == kinds[index]);
            assert(storage.message(id, row.id)->activityTarget == row.activityTarget);
        }
        auto anonymous = message("", now + 5);
        anonymous.activity = ActivityKind::Nick;
        anonymous.activityTarget = "anonymous-new-nick";
        storage.append(id, anonymous);
        const auto localId = storage.history(id).back().id;
        assert(localId.starts_with("local:"));
        auto restored = *storage.message(id, localId);
        assert(restored.activity == ActivityKind::Nick);
        assert(restored.activityTarget == "anonymous-new-nick");
        restored.activity = ActivityKind::Quit;
        restored.activityTarget.clear();
        storage.update(id, restored);
        assert(storage.message(id, localId)->activity == ActivityKind::Quit);
        assert(storage.message(id, localId)->activityTarget.empty());
        auto echo = message("local:activity-echo", now + 6);
        echo.own = echo.pending = true;
        echo.label = "activity-echo-label";
        echo.activity = ActivityKind::Part;
        storage.append(id, echo);
        assert(storage.message(id, echo.id)->sendStatus == SendStatus::Unknown);
        echo.id = "activity-echo-server";
        echo.pending = false;
        echo.activity = ActivityKind::Join;
        storage.append(id, echo, "", 0, true);
        assert(!storage.message(id, "local:activity-echo"));
        const auto confirmed = storage.message(id, echo.id);
        assert(confirmed->activity == ActivityKind::Join &&
               confirmed->sendStatus == SendStatus::Confirmed);

        // Nonempty destination classifications win; an empty one inherits the source.
        for (int index = 0; index < 2; ++index) {
            auto source = message("collision-" + std::to_string(index), now + 7 + index);
            source.activity = ActivityKind::Quit;
            source.activityTarget = "source-target";
            auto destination = source;
            destination.activity = index ? ActivityKind::Nick : ActivityKind::None;
            destination.activityTarget = index ? "destination-target" : "";
            storage.append("Libera/ActivityOld", source);
            storage.append("Libera/ActivityNew", destination);
        }
        storage.renameBuffer("Libera/ActivityOld", "Libera/ActivityNew");
        assert(storage.message("Libera/ActivityNew", "collision-0")->activity ==
               ActivityKind::Quit);
        assert(storage.message("Libera/ActivityNew", "collision-1")->activity ==
               ActivityKind::Nick);
        assert(storage.message("Libera/ActivityNew", "collision-0")->activityTarget ==
               "source-target");
        assert(storage.message("Libera/ActivityNew", "collision-1")->activityTarget ==
               "destination-target");
    }
    {
        Storage storage;
        Model model;
        storage.load(model);
        const auto history = storage.history(id, -1);
        const auto &loaded = model.buffers.at(id).messages;
        assert(history.size() == 7 && loaded.size() == 3);
        for (int index = 0; index < 5; ++index) {
            assert(history[index].activity == kinds[index] && history[index].text == "updated");
            assert(history[index].activityTarget == (index == 4 ? "new-nick" : ""));
        }
        assert(loaded[0].id == history[0].id && loaded[0].history &&
               loaded[1].id == history[5].id && loaded[1].activity == ActivityKind::Quit &&
               loaded[2].id == history[6].id && loaded[2].sendStatus == SendStatus::Confirmed);
        storage.clearHistory();
    }
}
static void activitySchema6Migration(const fs::path &database, const fs::path &config,
                                     int64_t now) {
    const string id = "Libera/#activity-migration";
    string localId;
    {
        Storage storage;
        Model model;
        storage.load(model);
        storage.clearHistory();
        model.settings.retentionDays = 0;
        model.settings.maxMessages = 1000;
        model.settings.presence = PresenceMode::Hidden;
        model.settings.channelPresence[id] = PresenceMode::All;
        auto buffer = sendBuffer(id);
        buffer.draft = "migration draft\nsecond line";
        buffer.readMarker = "migration-server-id";
        model.buffers[id] = buffer;
        model.activeBuffer = id;
        storage.save(model);
        auto row = message("migration-server-id", now, "joined the channel");
        row.presence = true; // Legacy text/presence must not infer an activity kind.
        row.reply = "opaque-reply-token";
        row.label = "opaque-label-token";
        row.reactions["👍"] = {"alice"};
        storage.append(id, row);
        auto local = message("", now + 1);
        local.own = local.pending = true;
        local.label = "migration-local-token";
        storage.append(id, local);
        localId = storage.history(id, -1).back().id;
        storage.queueSend(sendBuffer("Libera/#migration-queue"), "migration-queue-token",
                          "queued migration text");
    }
    const string profiles = contents(config / "config.toml");
    const string preferences = contents(config / "settings.toml");
    dropHistorySchema(database);
    sql(database, "ALTER TABLE messages DROP COLUMN activity; "
                  "ALTER TABLE messages DROP COLUMN activity_target; "
                  "ALTER TABLE buffers DROP COLUMN muted; "
                  "ALTER TABLE buffers DROP COLUMN archived; PRAGMA user_version=6;");
    const auto sequence =
        scalar(database, "SELECT seq FROM messages WHERE id='migration-server-id'");
    {
        Storage storage;
        assert(scalar(database, "PRAGMA user_version") == 12);
        assert(scalar(database, "SELECT COUNT(*) FROM messages WHERE activity<>0") == 0);
        assert(scalar(database, "SELECT seq FROM messages WHERE id='migration-server-id'") ==
               sequence);
        assert(scalar(database,
                      "SELECT COUNT(*) FROM queued_sends WHERE "
                      "label='migration-queue-token' AND text='queued migration text'") == 1);
        const auto row = storage.message(id, "migration-server-id");
        assert(row && row->activity == ActivityKind::None && row->presence &&
               row->text == "joined the channel" && row->reply == "opaque-reply-token" &&
               row->label == "opaque-label-token" &&
               row->reactions.at("👍") == vector<string>{"alice"});
        assert(row->activityTarget.empty());
        const auto local = storage.message(id, localId);
        assert(local && local->activity == ActivityKind::None && local->own && local->pending &&
               local->sendStatus == SendStatus::Unknown && local->label == "migration-local-token");
        Model loaded;
        storage.load(loaded);
        const auto &buffer = loaded.buffers.at(id);
        assert(loaded.activeBuffer == id && buffer.draft == "migration draft\nsecond line" &&
               buffer.readMarker == "migration-server-id" && buffer.messages.size() == 1 &&
               buffer.messages.back().id == localId &&
               buffer.messages.back().sendStatus == SendStatus::Unknown);
        assert(loaded.settings.presence == PresenceMode::Hidden &&
               loaded.settings.channelPresence.at(id) == PresenceMode::All);
        assert(loaded.buffers.at("Libera/#migration-queue").draft == "queued migration text");
        assert(contents(config / "config.toml") == profiles &&
               contents(config / "settings.toml") == preferences);
        auto nick = message("schema7-nick", now + 2, "opaque nick-change display text");
        nick.activity = ActivityKind::Nick;
        nick.activityTarget = "schema8-target";
        storage.append(id, nick);
    }
    dropHistorySchema(database);
    sql(database, "ALTER TABLE messages DROP COLUMN activity_target; "
                  "ALTER TABLE buffers DROP COLUMN muted; "
                  "ALTER TABLE buffers DROP COLUMN archived; PRAGMA user_version=7;");
    {
        Storage storage;
        assert(scalar(database, "PRAGMA user_version") == 12);
        const auto nick = storage.message(id, "schema7-nick");
        assert(nick && nick->activity == ActivityKind::Nick && nick->activityTarget.empty() &&
               nick->text == "opaque nick-change display text");
        assert(contents(config / "config.toml") == profiles &&
               contents(config / "settings.toml") == preferences);
        storage.clearHistory();
    }
}
static void chatFlags(const fs::path &database, const fs::path &config, int64_t now) {
    const string id = "Libera/#flag-migration";
    {
        Storage storage;
        Model model;
        storage.load(model);
        auto buffer = sendBuffer(id);
        assert(!buffer.muted && !buffer.archived);
        buffer.muted = buffer.archived = true;
        buffer.draft = "retained flag migration draft";
        buffer.readMarker = "flag-read";
        buffer.unread = 1;
        model.buffers[id] = buffer;
        model.activeBuffer = id;
        storage.save(model);
        storage.append(id, message("flag-read", now));
        storage.append(id, message("flag-unread", now + 1));
    }
    const string profiles = contents(config / "config.toml");
    const string preferences = contents(config / "settings.toml");
    dropHistorySchema(database);
    sql(database, "ALTER TABLE buffers DROP COLUMN muted; "
                  "ALTER TABLE buffers DROP COLUMN archived; PRAGMA user_version=8;");
    for (int reopen = 0; reopen < 2; ++reopen) {
        Storage storage;
        Model model;
        storage.load(model);
        const auto &buffer = model.buffers.at(id);
        assert(scalar(database, "PRAGMA user_version") == 12);
        assert(!buffer.muted && !buffer.archived);
        assert(model.activeBuffer == id && buffer.draft == "retained flag migration draft" &&
               buffer.readMarker == "flag-read" && buffer.unread == 1 &&
               buffer.messages.size() == 2 && buffer.messages[0].id == "flag-read" &&
               buffer.messages[1].id == "flag-unread");
        assert(scalar(database, "SELECT COUNT(*) FROM pragma_table_info('buffers') "
                                "WHERE name IN ('muted','archived') AND \"notnull\"=1 "
                                "AND dflt_value='0'") == 2);
        assert(contents(config / "config.toml") == profiles &&
               contents(config / "settings.toml") == preferences);
    }
    for (bool muted : {false, true})
        for (bool archived : {false, true}) {
            {
                Storage storage;
                Model model;
                storage.load(model);
                auto &buffer = model.buffers.at(id);
                buffer.muted = !muted;
                buffer.archived = !archived;
                storage.save(model);
                buffer.muted = muted;
                buffer.archived = archived;
                storage.save(model); // Update existing metadata, not just insert defaults.
            }
            {
                Storage storage;
                Model model;
                storage.load(model);
                auto &buffer = model.buffers.at(id);
                assert(buffer.muted == muted && buffer.archived == archived);
                buffer.muted = !muted;
                buffer.archived = !archived;
                storage.queueSend(buffer, "flag-queue", "queued flag text");
            } // queueSend must persist current flags without a later model save.
            {
                Storage storage;
                Model model;
                storage.load(model);
                const auto &buffer = model.buffers.at(id);
                assert(buffer.muted == !muted && buffer.archived == !archived &&
                       buffer.draft.find("queued flag text") != string::npos);
                auto fresh = sendBuffer("Libera/FlagQueueOnly");
                fresh.muted = muted;
                fresh.archived = archived;
                storage.closeBuffer(fresh);
                storage.queueSend(fresh, "flag-queue-only", "new flag buffer");
            }
            {
                Storage storage;
                Model model;
                storage.load(model);
                const auto &buffer = model.buffers.at("Libera/FlagQueueOnly");
                assert(buffer.muted == muted && buffer.archived == archived &&
                       buffer.draft == "new flag buffer");
            }
        }
    // Exhaust both orientations and equal inputs: neither source nor destination wins.
    for (bool sourceMuted : {false, true})
        for (bool destinationMuted : {false, true})
            for (bool sourceArchived : {false, true})
                for (bool destinationArchived : {false, true}) {
                    const string sourceId = "Libera/FlagOld", destinationId = "Libera/FlagNew";
                    {
                        Storage storage;
                        Model model;
                        storage.load(model);
                        auto source = sendBuffer(sourceId), destination = sendBuffer(destinationId);
                        source.kind = destination.kind = "query";
                        source.muted = sourceMuted;
                        destination.muted = destinationMuted;
                        source.archived = sourceArchived;
                        destination.archived = destinationArchived;
                        model.buffers[sourceId] = source;
                        model.buffers[destinationId] = destination;
                        storage.save(model);
                        storage.renameBuffer(sourceId, destinationId);
                    }
                    {
                        Storage storage;
                        Model model;
                        storage.load(model);
                        assert(!model.buffers.contains(sourceId));
                        const auto &merged = model.buffers.at(destinationId);
                        assert(merged.muted == (sourceMuted || destinationMuted) &&
                               merged.archived == (sourceArchived && destinationArchived));
                        storage.renameBuffer(destinationId, sourceId);
                    }
                    {
                        Storage storage;
                        Model model;
                        storage.load(model);
                        assert(!model.buffers.contains(destinationId));
                        const auto &renamed = model.buffers.at(sourceId);
                        assert(renamed.muted == (sourceMuted || destinationMuted) &&
                               renamed.archived == (sourceArchived && destinationArchived));
                    }
                }
    sql(database, "PRAGMA user_version=13;");
    bool rejected = false;
    try {
        Storage future;
    } catch (const std::runtime_error &) {
        rejected = true;
    }
    assert(rejected && scalar(database, "PRAGMA user_version") == 13);
    sql(database, "PRAGMA user_version=12;");
}
static void retainedMentions(const fs::path &database, int64_t now) {
    Storage storage;
    Model model;
    storage.load(model);
    storage.clearHistory();
    model.settings.retentionDays = 0;
    model.settings.maxMessages = 1000;
    const string a = "MentionsA/#one", b = "MentionsB/#two";
    model.buffers[a] = sendBuffer(a);
    model.buffers[b] = sendBuffer(b);
    model.buffers[a].draft = "private unfinished text";
    model.settings.bufferFools = {{a, true}, {b, false}};
    storage.save(model);
    Model restored;
    storage.load(restored);
    assert(restored.settings.bufferFools == model.settings.bufferFools);
    assert(!restored.settings.bufferFools.contains("Other/#missing"));
    map<string, Network> networks;
    networks["MentionsA"].nick = "current[";
    networks["MentionsB"].nick = "other";
    const auto revision = storage.revision();
    for (int i = 0; i < 225; ++i) {
        auto row = message("row-" + std::to_string(i), now, "hello CURRENT{!");
        row.nick = "mallory"; // Fool classification is independent of notification policy.
        storage.append(a, row);
    }
    assert(storage.revision() > revision && storage.history(a).size() == 200);
    assert(storage.history(a, -1).size() == 225);
    assert(storage.mentionSources({}).empty()); // Unknown nick never invents positives.
    networks["MentionsA"].caseMapping = "ascii";
    assert(storage.mentionSources(networks).empty());
    networks["MentionsA"].caseMapping = "rfc1459";
    auto sources = storage.mentionSources(networks);
    assert(sources.at(a).messages.size() == 225);
    const auto classifiedRevision = storage.revision();
    sources = storage.mentionSources(networks);
    assert(storage.revision() == classifiedRevision);
    const auto &rows = sources.at(a).messages;
    for (size_t i = 0; i < rows.size(); ++i) {
        assert(rows[i].mention && rows[i].id == "row-" + std::to_string(i));
        assert(rows[i].mentionNick == "current[");
        if (i)
            assert(rows[i - 1].storageSequence < rows[i].storageSequence);
    }
    auto later = message("watermark-later", now, "CURRENT{!");
    storage.append(a, later);
    assert(storage.mentionSources(networks).at(a).messages.size() == 226);
    storage.eraseMessage(a, later.id);
    auto edited = message("watermark-edited", now, "not a mention");
    storage.append(a, edited);
    assert(storage.mentionSources(networks).at(a).messages.size() == 225);
    edited.text = "CURRENT{!";
    storage.update(a, edited);
    assert(storage.mentionSources(networks).at(a).messages.size() == 226);
    storage.eraseMessage(a, edited.id);
    auto recorded = message("recorded", now, "past nickname, no longer known");
    recorded.mention = true;
    storage.append(a, recorded);
    recorded.mention = false;
    storage.update(a, recorded);
    assert(storage.message(a, recorded.id)->mention); // Stale live state cannot undo evidence.
    assert(storage.message(a, recorded.id)->mentionNick.empty());
    auto known = message("known-nick", now, "historic nick!");
    known.mention = true;
    known.mentionNick = "historic";
    storage.append(a, known);
    known.mention = false;
    known.mentionNick.clear();
    storage.update(a, known);
    assert(storage.message(a, known.id)->mentionNick == "historic");
    storage.eraseMessage(a, known.id);
    auto duplicate = message("same-id", now, "other");
    duplicate.time = "2023-11-14T22:13:20.900Z";
    duplicate.mention = true;
    storage.append(a, duplicate);
    duplicate.time = "2023-11-14T22:13:20.100Z";
    storage.append(b, duplicate);
    sources = storage.mentionSources(networks);
    const auto &left = sources.at(a).messages.back();
    const auto &right = sources.at(b).messages.back();
    assert(left.id == right.id && left.time != right.time &&
           left.storageSequence != right.storageSequence);
    for (const string id : {"own", "presence", "deleted", "boundary"}) {
        auto excluded = message(id, now, "CURRENT{suffix");
        excluded.mention = id != "boundary";
        excluded.own = id == "own";
        excluded.presence = id == "presence";
        excluded.deleted = id == "deleted";
        storage.append(a, excluded);
    }
    auto invalid = sendBuffer("Invalid/#missing-server");
    invalid.server.clear();
    model.buffers[invalid.id] = invalid;
    auto server = sendBuffer("MentionsA/*server*");
    server.kind = "server";
    model.buffers[server.id] = server;
    auto synthetic = sendBuffer("synthetic");
    synthetic.kind = "mentions";
    model.buffers[synthetic.id] = synthetic;
    storage.save(model);
    storage.append(invalid.id, duplicate);
    storage.append(server.id, duplicate);
    assert(!storage.sourceBuffer(invalid.id) && !storage.sourceBuffer(server.id) &&
           !storage.sourceBuffer(synthetic.id));
    networks["MentionsA"].nick = "renamed";
    sources = storage.mentionSources(networks);
    assert(storage.message(a, "row-0")->mentionNick == "current[");
    assert(storage.history(a, -1).front().mentionNick == "current[");
    assert(sources.size() == 2 && sources.at(a).messages.size() == 227);
    string renamed = "MentionsA/#renamed";
    storage.renameBuffer(a, renamed);
    auto redacted = *storage.message(renamed, "row-0");
    redacted.deleted = true;
    storage.update(renamed, redacted);
    sources = storage.mentionSources(networks);
    assert(!sources.contains(a) && sources.at(renamed).messages.size() == 226 &&
           storage.sourceBuffer(renamed)->target == "#renamed");
    storage.closeBuffer(*storage.sourceBuffer(renamed));
    model.buffers.erase(a);
    storage.save(model);
    assert(scalar(database, "SELECT COUNT(*) FROM buffers WHERE "
                            "id='MentionsA/#renamed' AND draft=''") == 1);
    const string closedRenamed = "MentionsA/#closed-renamed";
    storage.renameBuffer(renamed, closedRenamed);
    renamed = closedRenamed;
    assert(storage.sourceBuffer(renamed)->server == "MentionsA");
    assert(storage.mentionSources(networks).at(renamed).messages.size() == 226);
    storage.load(restored);
    assert(!restored.buffers.contains(renamed));
    storage.clearHistory(renamed);
    assert(!storage.mentionSources(networks).contains(renamed));
    model.settings.maxMessages = 1;
    storage.prune(model.settings);
    assert(storage.mentionSources(networks).size() <= 1);
    storage.clearHistory();
    assert(storage.mentionSources(networks).empty());
    auto unsaved = sendBuffer("MentionsA/#Unsaved");
    unsaved.draft = "never retain a closed draft";
    storage.append(unsaved.id, message("unsaved-mention", now, "renamed!"));
    assert(!storage.sourceBuffer(unsaved.id));
    // The live target can change case without changing its canonical buffer ID.
    unsaved.target = "#UNSAVED";
    unsaved.title = "authoritative live title";
    storage.closeBuffer(unsaved);
    const auto closedSource = storage.sourceBuffer(unsaved.id);
    assert(closedSource && closedSource->target == "#UNSAVED" &&
           closedSource->title == unsaved.title);
    assert(storage.mentionSources(networks).at(unsaved.id).messages.front().id ==
           "unsaved-mention");
    assert(scalar(database, "SELECT COUNT(*) FROM buffers WHERE "
                            "id='MentionsA/#Unsaved' AND draft=''") == 1);
    storage.load(restored);
    assert(!restored.buffers.contains(unsaved.id));
    storage.clearHistory();
    auto first = sendBuffer("MentionsA/#first-message");
    first.draft = "unsaved live draft";
    auto firstMessage = message("first-context", now, "renamed!");
    storage.append(first.id, firstMessage, "", 0, false, &first);
    assert(storage.sourceBuffer(first.id)->target == first.target);
    assert(storage.mentionSources(networks).at(first.id).messages.size() == 1);
    sql(database, "UPDATE buffers SET draft='durable draft',read_marker='durable marker',"
                  "unread=7 WHERE id='MentionsA/#first-message'");
    first.target = "#FIRST-MESSAGE";
    storage.update(first.id, firstMessage, "", 0, false, &first);
    assert(scalar(database, "SELECT COUNT(*) FROM buffers WHERE id='MentionsA/#first-message' "
                            "AND draft='durable draft' AND read_marker='durable marker' "
                            "AND unread=7 AND target='#FIRST-MESSAGE'") == 1);
    {
        Storage restarted;
        Model persisted;
        restarted.load(persisted);
        assert(persisted.buffers.at(first.id).target == "#FIRST-MESSAGE" &&
               persisted.buffers.at(first.id).messages.front().id == firstMessage.id);
        assert(restarted.mentionSources(networks).at(first.id).messages.size() == 1);
    }
    storage.clearHistory();
    assert(actualNickMention("hi nick!", "nick", "ascii") &&
           !actualNickMention("nickname", "nick", "ascii") &&
           !actualNickMention("CURRENT{!", "current[", "ascii") &&
           actualNickMention("CURRENT{!", "current[", "rfc1459"));
}
static void protocolHistoryStorage(const fs::path &database, int64_t now) {
    auto buffer = sendBuffer("HistoryNet/CaseSensitive");
    const HistoryCursor oldest{"Opaque-Z", "2026-11-01T00:00:00.001Z"};
    const HistoryCursor newest{"opaque-a", "2026-11-02T00:00:00.001Z"};
    int64_t gapId = 0;
    // Real v9 upgrade keeps existing rows, source metadata and private config bytes.
    const auto legacy = sendBuffer("HistoryNet/LegacyClosed");
    auto legacyOpen = sendBuffer("HistoryNet/LegacyOpen");
    auto cached = message("legacy-open-id", now - 20, "retained legacy cache");
    cached.time = "2026-11-01T03:04:05.678Z";
    string profiles, preferences;
    fs::path config;
    {
        Storage storage;
        config = storage.configDir();
        profiles = contents(config / "config.toml");
        preferences = contents(config / "settings.toml");
        storage.append(legacy.id, message("legacy-row", now), "", 0, false, &legacy);
        storage.closeBuffer(legacy);
        storage.append(legacyOpen.id, cached, "", 0, false, &legacyOpen);
        storage.append("HistoryNet/Dangling", message("dangling-id", now - 10));
    }
    const auto cachedRows = scalar(database, "SELECT COUNT(*) FROM messages");
    dropHistorySchema(database);
    sql(database, "PRAGMA user_version=9;");
    {
        Storage migrated;
        assert(scalar(database, "PRAGMA user_version") == 12);
        assert(migrated.message(legacy.id, "legacy-row"));
        assert(migrated.sourceBuffer(legacy.id)->target == legacy.target);
        assert(migrated.historyDiscarded(legacy.id) == std::optional<string>(""));
        assert(scalar(database, "SELECT COUNT(*) FROM messages") == cachedRows);
        const auto preserved = migrated.message(legacyOpen.id, cached.id);
        assert(preserved && preserved->text == cached.text && preserved->time == cached.time &&
               preserved->timestamp == cached.timestamp);
        migrated.loadHistoryState(legacyOpen);
        assert(legacyOpen.historyState.newest == (HistoryCursor{cached.id, cached.time}));
        assert(legacyOpen.historyState.oldest == HistoryCursor{} &&
               !legacyOpen.historyState.beginning && legacyOpen.historyState.gaps.empty());
        assert(migrated.historyDiscovery("HistoryNet").watermark.empty());
        Buffer closedState, danglingState;
        closedState.id = legacy.id;
        danglingState.id = "HistoryNet/Dangling";
        migrated.loadHistoryState(closedState);
        migrated.loadHistoryState(danglingState);
        assert(closedState.historyState.newest == HistoryCursor{} &&
               danglingState.historyState.newest == HistoryCursor{});
        auto joined = message("new-live-join", now);
        joined.time = "2026-11-02T03:04:05.678Z";
        joined.presence = true;
        joined.activity = ActivityKind::Join;
        migrated.append(legacyOpen.id, joined, "", 0, false, &legacyOpen);
        migrated.loadHistoryState(legacyOpen);
        assert(legacyOpen.historyState.newest == (HistoryCursor{cached.id, cached.time}));
        assert(contents(config / "config.toml") == profiles &&
               contents(config / "settings.toml") == preferences);
    }
    {
        Storage storage;
        Model model;
        storage.load(model);
        model.settings.retentionDays = 0;
        model.settings.maxMessages = 100000;
        model.buffers[buffer.id] = buffer;
        storage.save(model);
        // Mixed durable rows exercise the boundary selector, not a RAM transcript tail.
        const string boundaryBuffer = "HistoryNet/BoundarySelection";
        assert(!storage.latestHistoryBoundary(boundaryBuffer));
        storage.append(boundaryBuffer, message("opaque-server-id", now));
        assert(storage.latestHistoryBoundary(boundaryBuffer)->id == "opaque-server-id");
        const string originalTime = "2026-11-03T12:00:00.123456789+02:00";
        auto hidden = message("local-boundary", now + 1);
        hidden.time = originalTime;
        hidden.deleted = true;
        storage.append(boundaryBuffer, hidden);
        storage.append(boundaryBuffer, message("local:no-time", now + 2));
        storage.append(boundaryBuffer, message("local-no-time", now + 3));
        storage.append(boundaryBuffer, message("", now + 4));
        auto pending = message("pending-server-looking-id", now + 5);
        pending.time = originalTime;
        pending.own = pending.pending = true;
        storage.append(boundaryBuffer, pending);
        auto presence = message("latest-live-presence", now + 7);
        presence.time = originalTime;
        presence.presence = true;
        storage.append(boundaryBuffer, presence);
        auto selected = storage.latestHistoryBoundary(boundaryBuffer);
        assert(selected && selected->id == hidden.id && selected->time == originalTime);
        hidden.id.clear();
        hidden.timestamp = now + 6;
        int64_t anonymousSequence = 0;
        storage.append(boundaryBuffer, hidden, "", 0, false, nullptr, &anonymousSequence);
        selected = storage.latestHistoryBoundary(boundaryBuffer);
        assert(selected && selected->id == "local:" + std::to_string(anonymousSequence) &&
               selected->time == originalTime);
        const auto anonymousPosition = storage.messagePosition(boundaryBuffer, selected->id);
        assert(anonymousPosition && anonymousPosition->first == anonymousSequence &&
               anonymousPosition->second >= 0);
        Buffer repaired;
        repaired.id = "HistoryNet/PresenceRepair";
        const auto retain = [&](const string &id, int offset, bool isPresence) {
            auto row = message(id, now + offset);
            row.time = "2026-11-03T00:00:0" + std::to_string(offset) + ".000Z";
            row.presence = isPresence;
            storage.append(repaired.id, row);
            return HistoryCursor{row.id, row.time};
        };
        const auto contentA = retain("repair-A", 1, false);
        const auto olderPresence = retain("repair-older-presence", 2, true);
        const auto newerPresence = retain("repair-newer-presence", 4, true);
        const auto contentZ = retain("repair-Z-live-island", 5, false);
        repaired.historyState = {
            olderPresence, contentA, true, {{0, olderPresence, newerPresence, false}}};
        storage.saveHistoryState(repaired);
        const auto repairedGapId = repaired.historyState.gaps.front().id;
        storage.loadHistoryState(repaired);
        assert(repaired.historyState.oldest == contentA &&
               repaired.historyState.newest == contentA && !repaired.historyState.beginning);
        assert(repaired.historyState.gaps.size() == 1 &&
               repaired.historyState.gaps.front().id == repairedGapId &&
               repaired.historyState.gaps.front().older == contentA &&
               repaired.historyState.gaps.front().newer == contentZ);
        repaired.historyState.newest = olderPresence;
        storage.saveHistoryState(repaired);
        Storage reopenedRepair;
        reopenedRepair.loadHistoryState(repaired);
        assert(repaired.historyState.newest == contentA &&
               repaired.historyState.gaps.front().id == repairedGapId &&
               repaired.historyState.gaps.front().newer == contentZ);
        Buffer fenced;
        fenced.id = "HistoryNet/PresenceFence";
        auto fencePresence = message("fence-presence-T1", now + 1);
        fencePresence.time = "2026-11-03T00:00:01.000Z";
        fencePresence.presence = true;
        storage.append(fenced.id, fencePresence);
        auto laterIsland = message("fence-live-content-T3", now + 3);
        laterIsland.time = "2026-11-03T00:00:03.000Z";
        storage.append(fenced.id, laterIsland);
        fenced.historyState.oldest =
            fenced.historyState.newest = {fencePresence.id, fencePresence.time};
        storage.saveHistoryState(fenced);
        storage.loadHistoryState(fenced);
        assert(fenced.historyState.newest.id.empty() &&
               fenced.historyState.newest.time == fencePresence.time &&
               fenced.historyState.gaps.empty());
        Storage restartedFence;
        restartedFence.loadHistoryState(fenced);
        assert(fenced.historyState.newest.id.empty() &&
               fenced.historyState.newest.time == fencePresence.time &&
               fenced.historyState.newest.time != laterIsland.time);
        buffer.historyState = {oldest, newest, true, {{0, oldest, newest, false}}};
        auto write = storage.historyWrite();
        auto row = message(oldest.id, now);
        row.time = oldest.time;
        row.reactions["+1"] = {"bob"};
        storage.append(buffer.id, row);
        storage.saveHistoryState(buffer);
        storage.saveHistoryDiscovery("HistoryNet", {newest.time, oldest.time, false});
        write.commit();
        gapId = buffer.historyState.gaps.front().id;
        assert(gapId > 0);
    }
    {
        Storage storage;
        Model model;
        storage.load(model);
        auto &loaded = model.buffers.at(buffer.id);
        assert(loaded.historyState.oldest == oldest && loaded.historyState.newest == newest);
        assert(loaded.historyState.beginning && loaded.historyState.gaps.size() == 1);
        assert(loaded.historyState.gaps.front().id == gapId);
        assert(storage.historyDiscovery("HistoryNet").backfillUpper == oldest.time);
        assert(!storage.historyDiscarded(buffer.id));
        // Existing nested append/update transactions join the completed-page transaction.
        {
            auto abandoned = storage.historyWrite();
            auto row = *storage.message(buffer.id, oldest.id);
            row.deleted = true;
            storage.update(buffer.id, row);
            storage.append(buffer.id, message("uncommitted", now));
            loaded.historyState.gaps.clear();
            loaded.historyState.gaps.push_back({0, oldest, newest, true});
            storage.saveHistoryState(loaded);
            assert(loaded.historyState.gaps.front().id != gapId);
            storage.saveHistoryDiscovery("HistoryNet", {"not committed", "", true});
        }
        assert(!storage.message(buffer.id, "uncommitted"));
        auto original = storage.message(buffer.id, oldest.id);
        assert(original && !original->deleted && original->reactions.at("+1").front() == "bob");
        storage.loadHistoryState(loaded); // Caller repairs its in-memory page after rollback.
        assert(loaded.historyState.gaps.front().id == gapId);
        assert(storage.historyDiscovery("HistoryNet").watermark == newest.time);
        // A failed inner metadata write cannot leave the earlier page rows committed.
        sql(database, "CREATE TRIGGER fail_history_state BEFORE UPDATE ON history_state "
                      "BEGIN SELECT RAISE(ABORT,'injected history failure'); END;");
        bool failed = false;
        try {
            auto write = storage.historyWrite();
            storage.append(buffer.id, message("failed-page", now));
            storage.saveHistoryState(loaded);
            write.commit();
        } catch (const std::runtime_error &) {
            failed = true;
        }
        sql(database, "DROP TRIGGER fail_history_state;");
        assert(failed && !storage.message(buffer.id, "failed-page"));
        // Rollback to an inner savepoint leaves the containing page writable.
        {
            auto outer = storage.historyWrite();
            storage.append(buffer.id, message("outer-page", now));
            {
                auto inner = storage.historyWrite();
                storage.append(buffer.id, message("inner-page", now));
            }
            outer.commit();
        }
        assert(storage.message(buffer.id, "outer-page") &&
               !storage.message(buffer.id, "inner-page"));
        loaded.historyState.gaps.front().unavailable = true;
        storage.saveHistoryState(loaded);
        storage.closeBuffer(loaded);
        const auto discarded = storage.historyDiscarded(buffer.id);
        assert(discarded && !discarded->empty());
        storage.renameBuffer(buffer.id, "HistoryNet/Renamed");
        assert(!storage.historyDiscarded(buffer.id));
        assert(storage.historyDiscarded("HistoryNet/Renamed") == discarded);
    }
    {
        Storage storage;
        Buffer renamed;
        renamed.id = "HistoryNet/Renamed";
        storage.loadHistoryState(renamed);
        assert(renamed.historyState.oldest == oldest && renamed.historyState.beginning);
        assert(renamed.historyState.gaps.front().id == gapId &&
               renamed.historyState.gaps.front().unavailable);
        // Merging independent cached extents must preserve both gaps and the union.
        auto target = sendBuffer("HistoryNet/Destination");
        target.historyState = {
            {"later", "2026-11-03T00:00:00Z"}, {"latest", "2026-11-04T00:00:00Z"}, false, {}};
        storage.saveHistoryState(target);
        storage.renameBuffer(renamed.id, target.id);
        storage.loadHistoryState(target);
        assert(target.historyState.oldest == oldest);
        assert(target.historyState.newest.id == "latest" && target.historyState.gaps.size() == 2);
        assert(target.historyState.gaps.front().id == gapId);
        // Equal-time opaque IDs do not prove adjacent response slices are continuous.
        auto tiedSource = sendBuffer("HistoryNet/TiedSource");
        auto tiedDestination = sendBuffer("HistoryNet/TiedDestination");
        const string tie = "2026-11-03T12:00:00.123Z";
        tiedSource.historyState = {{"z", tie}, {"A", tie}, false, {}};
        tiedDestination.historyState = {{"y", tie}, {"B", tie}, false, {}};
        storage.saveHistoryState(tiedSource);
        storage.saveHistoryState(tiedDestination);
        storage.renameBuffer(tiedSource.id, tiedDestination.id);
        storage.loadHistoryState(tiedDestination);
        assert(tiedDestination.historyState.gaps.size() == 1);
        assert(tiedDestination.historyState.gaps.front().older.id == "A" &&
               tiedDestination.historyState.gaps.front().newer.id == "y");
        // Conversely an exact shared opaque boundary proves the seam, even in a tie.
        tiedSource.id = "HistoryNet/OverlapSource";
        tiedDestination.id = "HistoryNet/OverlapDestination";
        tiedSource.historyState = {{"z", tie}, {"A", tie}, false, {}};
        tiedDestination.historyState = {{"A", tie}, {"B", tie}, false, {}};
        storage.saveHistoryState(tiedSource);
        storage.saveHistoryState(tiedDestination);
        storage.renameBuffer(tiedSource.id, tiedDestination.id);
        storage.loadHistoryState(tiedDestination);
        assert(tiedDestination.historyState.gaps.empty());
        storage.clearHistory(target.id);
        storage.loadHistoryState(target);
        assert(target.historyState.oldest.id.empty() && target.historyState.gaps.empty() &&
               !target.historyState.beginning);
        assert(storage.historyDiscovery("HistoryNet").watermark == newest.time);
        // Retention invalidates proofs whose local source rows disappeared.
        storage.append(target.id, message("pruned", now));
        target.historyState = {oldest, newest, true, {{0, oldest, newest, false}}};
        storage.saveHistoryState(target);
        sql(database, "UPDATE messages SET sort_time=1 WHERE buffer='HistoryNet/Destination';");
        Model model;
        storage.load(model);
        model.settings.retentionDays = 1;
        storage.prune(model.settings);
        storage.loadHistoryState(target);
        assert(target.historyState.oldest.id.empty() && !target.historyState.beginning &&
               target.historyState.gaps.empty());
        storage.clearHistory();
        const auto discovery = storage.historyDiscovery("HistoryNet");
        assert(discovery.watermark.empty() && discovery.backfillUpper.empty() &&
               !discovery.backfillComplete);
        // Completed-page metadata is written before retention, never resurrected afterward.
        model.settings.retentionDays = 0;
        model.settings.maxMessages = 2;
        storage.prune(model.settings);
        storage.takeHistoryInvalidations();
        auto page = sendBuffer("HistoryNet/PageRetention");
        {
            auto outer = storage.historyWrite();
            storage.append(page.id, message("page-1", now));
            {
                auto inner = storage.historyWrite();
                storage.append(page.id, message("page-2", now + 1));
                storage.append(page.id, message("page-3", now + 2));
                inner.commit();
            }
            assert(storage.history(page.id, -1).size() == 3);
            page.historyState = {oldest, newest, true, {{0, oldest, newest, false}}};
            storage.saveHistoryState(page);
            outer.commit();
        }
        assert(storage.history(page.id, -1).size() == 2);
        assert(storage.takeHistoryInvalidations() == vector<string>{page.id});
        assert(storage.takeHistoryInvalidations().empty());
        storage.loadHistoryState(page);
        assert(page.historyState.oldest.id.empty() && page.historyState.gaps.empty() &&
               !page.historyState.beginning);
        // Forced trim failure rolls back rows and protocol proof in that same outer write.
        sql(database, "CREATE TRIGGER fail_page_trim BEFORE DELETE ON messages "
                      "WHEN OLD.buffer='HistoryNet/PageRetention' "
                      "BEGIN SELECT RAISE(ABORT,'injected page trim failure'); END;");
        bool trimFailed = false;
        try {
            auto write = storage.historyWrite();
            storage.append(page.id, message("page-4", now + 3));
            page.historyState = {oldest, newest, true, {{0, oldest, newest, false}}};
            storage.saveHistoryState(page);
            write.commit();
        } catch (const std::runtime_error &) {
            trimFailed = true;
        }
        sql(database, "DROP TRIGGER fail_page_trim;");
        assert(trimFailed && !storage.message(page.id, "page-4") &&
               storage.history(page.id, -1).size() == 2);
        assert(storage.takeHistoryInvalidations().empty());
        storage.loadHistoryState(page);
        assert(page.historyState.oldest.id.empty() && !page.historyState.beginning);
        storage.clearHistory();
    }
}
static void historyPlaybackOrdering(const fs::path &database, int64_t now) {
    auto source = sendBuffer("HistoryOrder/#cohort");
    const string time = "2026-11-03T12:00:00.123456789Z";
    auto ids = [](const auto &rows) {
        vector<string> result;
        for (const auto &row : rows)
            result.push_back(row.id);
        return result;
    };
    auto append = [&](Storage &storage, const string &id) {
        auto row = message(id, now);
        row.time = time;
        row.mention = true;
        storage.append(source.id, row, "", 0, false, &source, &row.storageSequence,
                       &row.historyOrder);
        assert(row.storageSequence > 0 && row.historyOrder >= 0);
        return row;
    };
    int64_t originalSequence = 0;
    {
        Storage storage;
        Model model;
        storage.load(model);
        model.settings.retentionDays = 0;
        model.settings.maxMessages = 100000;
        model.buffers[source.id] = source;
        storage.save(model);
        originalSequence = append(storage, "B").storageSequence;
        append(storage, "D");
        source.historyState.newest = {"B", time};
        storage.saveHistoryState(source);
    }
    // An interim v10 database gains ranks without touching its existing protocol metadata.
    dropHistoryOrderSchema(database);
    sql(database, "PRAGMA user_version=10;");
    const vector<string> expected{"pre1", "pre2", "A", "D", "X", "C", "B", "tailY", "tailZ"};
    {
        Storage storage;
        assert(scalar(database, "PRAGMA user_version") == 12);
        storage.loadHistoryState(source);
        assert(source.historyState.newest == (HistoryCursor{"B", time}));
        assert(storage.message(source.id, "B")->historyOrder == originalSequence);
        // A native optimistic own row can preexist logically while first becoming durable now.
        const string optimisticBuffer = "HistoryOrder/#optimistic";
        auto live = message("live-before-own", now);
        live.time = time;
        storage.append(optimisticBuffer, live);
        {
            auto write = storage.historyWrite();
            auto own = message("materialized-own", now);
            own.time = time;
            own.own = true;
            storage.append(optimisticBuffer, own);
            write.reconcileOrder(optimisticBuffer, {own.id}, true, {own.id});
            write.commit();
        }
        assert(ids(storage.history(optimisticBuffer)) ==
               (vector<string>{"live-before-own", "materialized-own"}));
        // Native order, not merely logical existingness, also proves placement before a neighbor.
        {
            auto write = storage.historyWrite();
            auto own = message("materialized-before", now);
            own.time = time;
            own.own = true;
            storage.append(optimisticBuffer, own);
            write.reconcileOrder(optimisticBuffer, {own.id}, true,
                                 {own.id, "live-before-own", "materialized-own"});
            write.commit();
        }
        assert(ids(storage.history(optimisticBuffer)) ==
               (vector<string>{"materialized-before", "live-before-own", "materialized-own"}));
        // Receipt/fallback seconds are not authoritative playback time, even with a msgid.
        const string unknownBuffer = "HistoryOrder/#unknown-time";
        storage.append(unknownBuffer, message("untimed-live", now));
        {
            auto write = storage.historyWrite();
            auto untimed = message("untimed-history", now);
            untimed.history = true;
            storage.append(unknownBuffer, untimed);
            untimed.id = "malformed-time-history";
            untimed.time = "not-a-server-time";
            storage.append(unknownBuffer, untimed);
            assert(write
                       .reconcileOrder(unknownBuffer, {"malformed-time-history", "untimed-history"},
                                       true)
                       .empty());
            write.commit();
        }
        assert(ids(storage.history(unknownBuffer)) ==
               (vector<string>{"untimed-live", "untimed-history", "malformed-time-history"}));
        assert(storage.messagePosition(unknownBuffer, "untimed-history")->second == 1 &&
               storage.messagePosition(unknownBuffer, "malformed-time-history")->second == 2);
        assert(scalar(database,
                      "SELECT COUNT(*) FROM messages WHERE "
                      "buffer='HistoryOrder/#unknown-time' AND tie_order_confirmed<>0") == 0);
        // First confirmed playback may correct provisional live-arrival order.
        {
            auto write = storage.historyWrite();
            write.reconcileOrder(source.id, {"D", "B", "D"}, false);
            write.commit();
        }
        assert(ids(storage.history(source.id)) == (vector<string>{"D", "B"}));
        // LATEST uses the nearest known next/previous neighbors, not lexical IDs.
        {
            auto write = storage.historyWrite();
            append(storage, "A");
            append(storage, "C");
            write.reconcileOrder(source.id, {"A", "D", "C", "B"}, false);
            write.commit();
        }
        assert(ids(storage.history(source.id)) == (vector<string>{"A", "D", "C", "B"}));
        // An unanchored BEFORE page prepends in wire order.
        {
            auto write = storage.historyWrite();
            append(storage, "pre1");
            append(storage, "pre2");
            write.reconcileOrder(source.id, {"pre1", "pre2"}, true);
            write.commit();
        }
        // AROUND still anchors inside the existing cohort rather than blindly prepending.
        {
            auto write = storage.historyWrite();
            append(storage, "X");
            write.reconcileOrder(source.id, {"D", "X", "C"}, true);
            write.commit();
        }
        // Unanchored LATEST/AFTER order appends, without reverting earlier confirmed order.
        {
            auto write = storage.historyWrite();
            append(storage, "tailY");
            append(storage, "tailZ");
            const auto ranks = write.reconcileOrder(source.id, {"tailY", "tailZ"}, false);
            assert(ranks.size() == expected.size() && std::is_sorted(ranks.begin(), ranks.end()));
            write.commit();
        }
        {
            auto write = storage.historyWrite();
            write.reconcileOrder(source.id, {"B", "D"}, false); // Contradicts confirmed D,B.
            write.commit();
        }
        auto rows = storage.history(source.id);
        assert(ids(rows) == expected);
        for (size_t i = 0; i < rows.size(); ++i) {
            assert(rows[i].historyOrder == static_cast<int64_t>(i));
            assert(rows[i].timestamp == now && rows[i].time == time);
        }
        assert(scalar(database, "SELECT COUNT(*) FROM messages WHERE "
                                "buffer='HistoryOrder/#cohort' AND tie_order_confirmed=1") == 9);
        auto existing = *storage.message(source.id, "B");
        existing.historyOrder = -1;
        existing.text = "updated without moving its confirmed position";
        storage.update(source.id, existing, "", 0, false, nullptr, &existing.storageSequence,
                       &existing.historyOrder);
        assert(existing.storageSequence == originalSequence && existing.historyOrder == 6);
        assert(storage.messagePosition(source.id, "B") ==
               (std::pair<int64_t, int64_t>{originalSequence, 6}));
        assert(ids(storage.history(source.id, 3, "C")) == (vector<string>{"A", "D", "X"}));
        assert(storage.history(source.id, 3, time).empty());
        assert(storage.latestHistoryBoundary(source.id)->id == "tailZ");
        map<string, Network> networks;
        networks[source.server].nick = "self";
        assert(ids(storage.mentionSources(networks).at(source.id).messages) == expected);
        // Failure after reconciliation rolls back the page, all rank shifts and protocol state.
        sql(database, "CREATE TRIGGER fail_after_order BEFORE UPDATE ON history_state "
                      "WHEN NEW.buffer='HistoryOrder/#cohort' "
                      "BEGIN SELECT RAISE(ABORT,'injected post-order metadata failure'); END;");
        bool failed = false;
        try {
            auto write = storage.historyWrite();
            append(storage, "rollback");
            write.reconcileOrder(source.id, {"rollback"}, true);
            source.historyState.beginning = true;
            storage.saveHistoryState(source);
            write.commit();
        } catch (const std::runtime_error &) {
            failed = true;
        }
        sql(database, "DROP TRIGGER fail_after_order;");
        assert(failed && !storage.message(source.id, "rollback"));
        rows = storage.history(source.id);
        assert(ids(rows) == expected);
        for (size_t i = 0; i < rows.size(); ++i)
            assert(rows[i].historyOrder == static_cast<int64_t>(i));
        storage.loadHistoryState(source);
        assert(!source.historyState.beginning && source.historyState.newest.id == "B");
        storage.read(source.id, "D");
    }
    {
        Storage storage;
        Model model;
        storage.load(model);
        const auto &restored = model.buffers.at(source.id);
        assert(expected == ids(restored.messages));
        assert(ids(storage.history("HistoryOrder/#optimistic")) ==
               (vector<string>{"materialized-before", "live-before-own", "materialized-own"}));
        assert(storage.message(source.id, "B")->storageSequence == originalSequence);
        const string renamed = "HistoryOrder/#renamed";
        storage.renameBuffer(source.id, renamed);
        storage.load(model);
        assert(model.buffers.at(renamed).readMarker == "D" &&
               model.buffers.at(renamed).unread == 5);
        assert(ids(storage.history(renamed)) == expected);
        model.settings.retentionDays = 0;
        model.settings.maxMessages = 3;
        storage.prune(model.settings);
        assert(ids(storage.history(renamed)) == (vector<string>{"B", "tailY", "tailZ"}));
        assert(!storage.messagePosition(renamed, "D"));
        assert(storage.messagePosition(renamed, "B")->first == originalSequence);
        storage.clearHistory();
    }
}
static void localDataScopes(const fs::path &root, int64_t now) {
    ::setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
    ::setenv("XDG_DATA_HOME", (root / "data").c_str(), 1);
    const auto config = root / "config" / "blastirc";
    const auto profiles = config / "config.toml", preferences = config / "settings.toml";
    const auto database = root / "data" / "blastirc" / "state.sqlite3";
    const string room = "Libera/#ops/*server*", closed = "Libera/#clear-closed/*server*",
                 log = "Libera/*server*", orphanLog = "Orphan/*server*",
                 closedLog = "Libera/legacy-server-log", orphanRoom = "Orphan/#ops/*server*";
    Storage storage;
    Model model;
    storage.load(model);
    auto &chat = model.buffers.emplace(room, sendBuffer(room)).first->second;
    chat.draft = "editable draft";
    model.buffers[log] = sendBuffer(log);
    model.buffers[log].kind = "server";
    model.activeBuffer = room;
    model.settings.sound = false;
    storage.saveProfiles(model);
    assert(fs::exists(profiles) && !fs::exists(preferences));
    assert(scalar(database, "SELECT count(*) FROM buffers") == 0);
    model.firstRun = false;
    storage.save(model);
    const auto savedProfiles = contents(profiles);

    auto row = message("row", now);
    row.reactions["reaction"] = {"alice"};
    storage.append(room, row);
    storage.append(closed, row);
    storage.closeBuffer(sendBuffer(closed));
    const auto cutoff = storage.historyDiscarded(closed);
    assert(cutoff && !cutoff->empty());
    storage.append(log, row);
    storage.append(orphanLog, row); // Crash before the log's source metadata was materialized.
    storage.append(orphanRoom, row);
    storage.append(closedLog, row);
    auto legacy = sendBuffer(closedLog);
    legacy.kind = "server";
    storage.closeBuffer(legacy);
    chat.readMarker = row.id;
    chat.unread = 2;
    storage.read(log, row.id);
    chat.historyState.oldest = chat.historyState.newest = {row.id, ""};
    chat.historyState.gaps.push_back({0, {}, {row.id, ""}, false});
    storage.saveHistoryState(chat);
    storage.saveHistoryDiscovery("Libera", {"watermark", "", true});
    storage.queueSend(chat, "submission", "queued text");
    auto ghost = message("local:ghost", now);
    ghost.own = ghost.pending = true;
    ghost.label = "submission.part1";
    storage.append(room, ghost);
    auto unbacked = ghost;
    unbacked.id = "local:unbacked";
    unbacked.label = "no-backup";
    unbacked.text = "unconfirmed text";
    storage.append(room, unbacked);
    unbacked.pending = false;
    storage.update(room, unbacked);

    storage.clearLocalData(LocalData::ServerLogs);
    for (const auto &id : {log, orphanLog, closedLog})
        assert(storage.history(id, -1).empty());
    assert(storage.message(room, row.id) && storage.message(closed, row.id));
    assert(storage.message(orphanRoom, row.id));
    assert(storage.historyDiscovery("Libera").watermark == "watermark");
    assert(scalar(database, "SELECT count(*) FROM buffers WHERE read_marker<>''") == 1);
    assert(scalar(database, "SELECT sum(unread) FROM buffers") == 2);
    assert(scalar(database, "SELECT count(*) FROM history_gaps") == 1);
    storage.append(orphanLog, row);
    storage.append(closedLog, row);
    storage.read(log, "log-marker");

    storage.clearLocalData(LocalData::ChatHistory);
    assert(storage.history(room).empty() && storage.history(closed, -1).empty());
    assert(!storage.message(room, row.id) && storage.message(room, ghost.id) &&
           storage.message(room, unbacked.id));
    assert(!storage.message(orphanRoom, row.id));
    assert(storage.historyDiscarded(closed) == cutoff && storage.message(orphanLog, row.id) &&
           storage.message(closedLog, row.id));
    assert(storage.historyDiscovery("Libera").watermark.empty());
    assert(scalar(database, "SELECT count(*) FROM reactions") == 2);
    assert(scalar(database, "SELECT count(*) FROM history_state") == 0);
    assert(scalar(database, "SELECT count(*) FROM history_gaps") == 0);
    assert(scalar(database, "SELECT count(*) FROM buffers WHERE read_marker='log-marker'") == 1);
    assert(scalar(database, "SELECT count(*) FROM buffers WHERE read_marker='row'") == 0);
    assert(scalar(database, "SELECT sum(unread) FROM buffers") == 0);
    assert(scalar(database, "SELECT count(*) FROM queued_sends") == 1);
    Model loaded;
    {
        Storage reopened;
        reopened.load(loaded);
        auto &restored = loaded.buffers.at(room);
        assert(restored.draft == "queued text\neditable draft");
        assert(restored.messages.size() == 1 && restored.messages.front().id == unbacked.id &&
               restored.messages.front().sendStatus == SendStatus::Unknown);
        restored.draft = restored.messages.front().text + "\n" + restored.draft;
        reopened.recoverSend(restored, unbacked.label);
    }
    storage.load(loaded);
    assert(loaded.buffers.at(room).messages.empty() &&
           loaded.buffers.at(room).draft == "unconfirmed text\nqueued text\neditable draft");

    storage.queueSend(loaded.buffers.at(room), "submission", "do not recover");
    storage.append(room, ghost);
    ghost.id = "local:orphan";
    ghost.label = "missing-backup";
    storage.append(room, ghost);
    ghost.pending = false;
    storage.update(room, ghost); // A provisional row even after the visual pending timeout.
    row.own = true;
    row.label = "submission";
    storage.append(room, row, "", 0, true);
    auto sent = message("local:sent", now);
    sent.own = true;
    sent.sendStatus = SendStatus::Sent;
    storage.append(room, sent);
    storage.clearLocalData(LocalData::Drafts);
    storage.load(loaded);
    assert(loaded.buffers.at(room).draft.empty() && storage.history(room, -1).size() == 2);
    assert(storage.message(room, row.id)->sendStatus == SendStatus::Confirmed);
    assert(storage.message(room, sent.id)->sendStatus == SendStatus::Sent);

    irc::saveSts("irc.example.test", 6697, 3600);
    auto defaults = defaultSettings();
    storage.saveSettings(defaults);
    assert(contents(profiles) == savedProfiles && storage.message(room, row.id));
    storage.load(loaded);
    assert(!loaded.firstRun && loaded.settings.sound && loaded.settings.cues.contains("mention") &&
           loaded.settings.bindings == defaults.bindings);
    const auto savedPreferences = contents(preferences);
    storage.clearLocalData(LocalData::Connections);
    assert(!fs::exists(profiles) && contents(preferences) == savedPreferences &&
           irc::loadSts("irc.example.test") && storage.message(room, row.id));
    storage.load(loaded);
    assert(loaded.firstRun && loaded.buffers.contains(room));

    const auto theme = config / "noctalia.toml", external = root / "client.key";
    std::ofstream(theme) << "external theme";
    std::ofstream(external) << "external key";
    std::ofstream(profiles) << "[invalid file";
    bool refused = false;
    try {
        storage.load(loaded);
    } catch (const std::runtime_error &) {
        try {
            storage.saveProfiles(model);
        } catch (const std::runtime_error &) {
            refused = true;
        }
    }
    assert(refused && contents(profiles) == "[invalid file");

    {
        auto write = storage.historyWrite();
        bool rejected = false;
        try {
            storage.preflightLocalReset();
        } catch (const std::runtime_error &error) {
            rejected = std::string_view(error.what()).find("active database write") !=
                       std::string_view::npos;
        }
        assert(rejected && storage.message(room, row.id));
    }

    const auto savedConfig = config.parent_path() / "test-saved-config";
    const auto savedPolicies = contents(config / "sts");
    fs::rename(config, savedConfig);
    fs::create_directory_symlink(savedConfig, config);
    for (const bool preflightOnly : {true, false}) {
        bool rejected = false;
        try {
            if (preflightOnly)
                storage.preflightLocalReset();
            else
                storage.clearLocalData(LocalData::All);
        } catch (const std::runtime_error &error) {
            rejected = std::string_view(error.what()).find("symlinked configuration") !=
                       std::string_view::npos;
        }
        assert(rejected && storage.message(room, row.id) && contents(profiles) == "[invalid file" &&
               contents(preferences) == savedPreferences &&
               contents(config / "sts") == savedPolicies);
    }
    fs::remove(config);
    fs::rename(savedConfig, config);

    const auto sts = config / "sts", savedSts = config / "test-saved-sts";
    fs::rename(sts, savedSts);
    fs::create_directory(sts); // Removal must fail, not recurse into an unowned directory.
    bool partial = false;
    try {
        storage.clearLocalData(LocalData::All);
    } catch (const std::runtime_error &error) {
        const std::string_view detail(error.what());
        partial = detail.find("STS policies") != string::npos &&
                  detail.find("local database was cleared") != string::npos;
    }
    assert(partial && contents(profiles) == "[invalid file" &&
           contents(preferences) == savedPreferences && !storage.message(room, row.id));
    fs::remove(sts);
    fs::rename(savedSts, sts);
    storage.clearLocalData(LocalData::All);
    assert(!fs::exists(profiles) && !fs::exists(preferences) && !fs::exists(config / "sts"));
    assert(contents(theme) == "external theme" && contents(external) == "external key");
    assert(fs::exists(database) && !storage.historyDiscarded(closed) &&
           storage.takeHistoryInvalidations().empty());
    for (const auto *table : {"messages", "reactions", "buffers", "queued_sends", "closed_buffers",
                              "history_state", "history_gaps", "history_discovery", "meta"})
        assert(scalar(database, (string("SELECT count(*) FROM ") + table).c_str()) == 0);
    Storage fresh;
    fresh.load(loaded);
    assert(loaded.firstRun && loaded.buffers.empty() && loaded.activeBuffer.empty());
}

int main() {
    char temporary[] = "/tmp/blastirc-storage-XXXXXX";
    const char *directory = ::mkdtemp(temporary);
    assert(directory);
    const fs::path root(directory), config = root / "config" / "blastirc",
                                    data = root / "data" / "blastirc";
    ::setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
    ::setenv("XDG_DATA_HOME", (root / "data").c_str(), 1);
    const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    try {
        Model expected;
        {
            Storage storage;
            assert(!fs::exists(config / "config.toml"));
            storage.load(expected);
            assert(expected.firstRun && expected.servers.at(0).tls);
            assert(expected.settings.bindings.at("search") == "C-s");
            assert(expected.settings.bindings.at("conversation-back") == "C-o" &&
                   expected.settings.bindings.at("conversation-forward") == "C-i");
            assert(expected.settings.sendMotion == SendMotion::Flight &&
                   expected.settings.sendReveal == SendReveal::None);
            assert(expected.settings.incomingMotion == SendMotion::Pop &&
                   expected.settings.incomingReveal == SendReveal::None &&
                   expected.settings.incomingDurationMs == 280.f &&
                   expected.settings.foolDisplay == FoolDisplay::Minimized &&
                   !expected.settings.foolUnread && expected.settings.messageWrapColumns == 80);
            assert(expected.settings.linkPreviews == PreviewMode::Click &&
                   expected.settings.imagePreviews == PreviewMode::Click &&
                   expected.settings.mediaPreviews == PreviewMode::Click &&
                   expected.settings.previewMaxHeight == 120 &&
                   expected.settings.previewMediaMaxMiB == 64 &&
                   expected.settings.previewMediaMaxSeconds == 600);
            assert(!expected.settings.previewAllowLocal);
            assert(!fs::exists(config / "settings.toml")); // Loading never writes preferences.
            assert(expected.settings.fx == FxLevel::Subtle && expected.settings.sound);
            assert(expected.settings.cursorShape == "line" &&
                   expected.settings.cursorBlink == "step" && !expected.settings.cursorTrail &&
                   expected.settings.cursorTrailStyle == "classic" &&
                   expected.settings.cursorSmoothBlinkMs == 1500);
            assert(expected.settings.peopleMode == SidebarMode::Hidden);
            assert(expected.settings.opacity == 1.f && !expected.settings.transparency &&
                   expected.settings.messageLayouts.empty());
            assert(expected.settings.channelMessageLayout == MessageLayout::Stacked &&
                   expected.settings.directMessageLayout == MessageLayout::Bubbles);
            storage.save(expected); // Closing an unfinished wizard preserves preferences only.
            assert(!fs::exists(config / "config.toml") && fs::exists(config / "settings.toml"));
            Model unfinished;
            storage.load(unfinished);
            assert(unfinished.firstRun && unfinished.settings.peopleMode == SidebarMode::Hidden);
            assert(unfinished.settings.fx == FxLevel::Subtle && unfinished.settings.sound);
            presentation(unfinished.settings, expected.settings);
            expected.firstRun = false; // The wizard has now been completed explicitly.
            expected.identities[0].password = "secret ' \\\"\npassword";
            expected.identities[0].certificate = "/tmp/client.pem";
            expected.identities[0].key = "/tmp/client.key";
            expected.servers[0].sasl = "PLAIN";
            expected.servers[0].password = "network secret";
            expected.servers[0].bouncer = "soju";
            expected.servers[0].channels = {"#a", "#b"};
            expected.servers[0].alias = "Friendly network 猫";
            expected.servers[0].avatar = "/tmp/network avatar.png";
            auto &s = expected.settings;
            s.fx = FxLevel::Subtle;
            s.presence = PresenceMode::Hidden;
            s.opacity = .63f;
            s.transparency = true;
            s.scale = 1.25f;
            s.messageScale = 1.6f;
            s.sendMotion = SendMotion::Rise;
            s.sendReveal = SendReveal::Fade;
            s.incomingMotion = SendMotion::Slide;
            s.incomingReveal = SendReveal::Fade;
            s.incomingDurationMs = 1250.f;
            s.foolDisplay = FoolDisplay::Blurred;
            s.foolUnread = true;
            s.messageWrapColumns = 120;
            s.masterVolume = .7f;
            s.gravity = -12;
            s.drag = 2.5f;
            s.shake = .4f;
            s.reducedMotion = true;
            s.sound = false;
            s.typingSend = false;
            s.typingReceive = false;
            s.timestamps = false;
            s.avatars = false;
            s.peopleMode = SidebarMode::Expanded;
            s.retentionDays = 0;
            s.maxMessages = 3;
            s.uiFont = "UI test font";
            s.messageFont = "message test font";
            s.uiFontWeight = 300;
            s.messageFontWeight = 700;
            s.nickPalette = "VIVID";
            s.avatarStyle = "initials";
            s.themeSource = "builtin";
            s.pitchVariation = "fixed";
            s.bindings["send"] = "C-c C-c";
            s.cues["mention"] = {false, .2f, 1.3f};
            s.nickColors["alice"] = "#ff00aa";
            s.friends = {"alice", "bob"};
            s.fools = {"mallory"};
            s.channelPresence["Libera/#a"] = PresenceMode::All;
            s.channelMessageLayout = MessageLayout::Bubbles;
            s.directMessageLayout = MessageLayout::Stacked;
            s.messageLayouts = {{"Libera/#a", MessageLayout::Irc},
                                {"Other/#a", MessageLayout::Irc}};
            s.pinnedBuffers = {"Other/#a", "Libera/#a"};
            s.workspace = {{"focused", "2"},
                           {"1", "Libera/#b"},
                           {"2", "Libera/#a"},
                           {"visible_buffers", "Libera/#a|Libera/#b"},
                           {"future_layout", "retained"}};
            Buffer buffer;
            buffer.id = "Libera/#a";
            buffer.server = "Libera";
            buffer.target = "#a";
            buffer.title = "Channel A";
            buffer.draft = "unfinished\nsecond line";
            buffer.topic = "local topic";
            buffer.readMarker = "1970-01-01T00:00:00Z";
            buffer.unread = 7;
            expected.buffers.emplace(buffer.id, buffer);
            expected.activeBuffer = buffer.id;
            storage.save(expected);
            storage.append(buffer.id, message("a1", 100));
            storage.append(buffer.id, message("a2", 200));
            storage.append("Libera/#b", message("b3", 300));
            storage.append(buffer.id, message("a4", 400));
            const auto tail = storage.history(buffer.id);
            assert(tail.size() == 2 && tail[0].id == "a2" && tail[1].id == "a4");
            const auto older = storage.history(buffer.id, 200, "a4");
            assert(older.size() == 1 && older[0].id == "a2");
            assert(storage.history(buffer.id, 200, "1970-01-01T00:05:00Z").size() == 1);
            assert(storage.history("Libera/#b").size() == 1); // Global cap, not a per-channel cap.
            storage.clearHistory();
            auto historyOne = message("history-1", now), historyTwo = message("history-2", now + 1);
            historyOne.own = historyTwo.own = true;
            historyOne.label = historyTwo.label = "history-batch-label";
            storage.append(buffer.id, historyOne);
            storage.append(buffer.id, historyTwo);
            assert(storage.history(buffer.id).size() ==
                   2); // A shared batch label is not an echo correlation ID.
            storage.clearHistory();
            auto quoted = message("quote' OR 1=1--", now, "quoted text");
            quoted.time = "2026-11-04T12:00:00.123Z";
            quoted.reply = "parent-id";
            quoted.label = "quote-label";
            quoted.own = true;
            quoted.mention = true;
            quoted.action = true;
            quoted.presence = true;
            quoted.reactions["👍"] = {"alice", "bob"};
            storage.append(buffer.id, quoted);
            auto ghost = message("temporary", now + 1, "own echo");
            ghost.own = true;
            ghost.pending = true;
            ghost.label = "echo-label";
            storage.append(buffer.id, ghost);
            ghost.pending = false;
            storage.update(buffer.id, ghost); // Auto-solidification must retain echo correlation.
            ghost.id = "server-id";
            ghost.pending = false;
            storage.update(buffer.id, ghost);
            const auto rows = storage.history(buffer.id, -1);
            assert(rows.size() == 2 && rows[1].id == "server-id" && !rows[1].pending);
            assert(rows[0].id == quoted.id && rows[0].reply == "parent-id" &&
                   rows[0].reactions == quoted.reactions);
            assert(rows[0].time == quoted.time && rows[0].timestamp == now && rows[0].history &&
                   rows[0].arrival == 0);
            assert(rows[0].own && rows[0].mention && rows[0].action && rows[0].presence);
            const auto exact = storage.message(buffer.id, quoted.id);
            assert(exact && exact->reply == quoted.reply && exact->reactions == quoted.reactions);
            assert(!storage.message(buffer.id, "not-present") &&
                   !storage.message("different-buffer", quoted.id));
            auto rejected = message("rejected-local", now + 2, "unsent");
            rejected.own = true;
            rejected.pending = true;
            rejected.label = "rejected-label";
            storage.append(buffer.id, rejected);
            storage.eraseMessage("different-buffer", rejected.id);
            assert(storage.message(buffer.id, rejected.id));
            storage.eraseMessage(buffer.id, rejected.id);
            assert(!storage.message(buffer.id, rejected.id) &&
                   storage.history(buffer.id, -1).size() == 2);
            storage.append(buffer.id, message("", now + 3, "anonymous rejected row"));
            const auto anonymousId = storage.history(buffer.id, 1).back().id;
            storage.eraseMessage(buffer.id, anonymousId);
            assert(!storage.message(buffer.id, anonymousId));
            storage.save(expected);
        }
        {
            Storage storage;
            Model actual;
            storage.load(actual);
            assert(!actual.firstRun && actual.activeBuffer == expected.activeBuffer);
            assert(actual.identities[0].password == expected.identities[0].password);
            assert(actual.identities[0].certificate == expected.identities[0].certificate &&
                   actual.identities[0].key == expected.identities[0].key);
            assert(actual.servers[0].password == expected.servers[0].password &&
                   actual.servers[0].channels == expected.servers[0].channels);
            assert(actual.servers[0].alias == expected.servers[0].alias &&
                   actual.servers[0].avatar == expected.servers[0].avatar);
            const auto &a = actual.settings;
            const auto &e = expected.settings;
            presentation(a, e);
            assert(a.pinnedBuffers == e.pinnedBuffers);
            assert(a.fx == e.fx && a.presence == e.presence && a.opacity == e.opacity &&
                   a.scale == e.scale && a.masterVolume == e.masterVolume);
            assert(a.transparency == e.transparency && a.messageLayouts == e.messageLayouts &&
                   !a.messageLayouts.contains("Libera/#b"));
            assert(a.gravity == e.gravity && a.drag == e.drag && a.shake == e.shake &&
                   a.reducedMotion == e.reducedMotion && a.sound == e.sound);
            assert(a.typingSend == e.typingSend && a.typingReceive == e.typingReceive &&
                   a.timestamps == e.timestamps && a.avatars == e.avatars);
            assert(a.uiFont == e.uiFont && a.messageFont == e.messageFont &&
                   a.nickPalette == e.nickPalette && a.avatarStyle == e.avatarStyle);
            assert(a.uiFontWeight == e.uiFontWeight && a.messageFontWeight == e.messageFontWeight);
            assert(a.themeSource == e.themeSource && a.pitchVariation == e.pitchVariation &&
                   a.retentionDays == 0 && a.maxMessages == 3);
            assert(a.bindings == e.bindings && a.nickColors == e.nickColors &&
                   a.workspace == e.workspace && a.peopleMode == e.peopleMode &&
                   a.channelPresence == e.channelPresence);
            assert(a.friends == e.friends && a.fools == e.fools);
            assert(!a.cues.at("mention").enabled && a.cues.at("mention").volume == .2f &&
                   a.cues.at("mention").pitch == 1.3f);
            const auto &buffer = actual.buffers.at("Libera/#a");
            assert(buffer.draft == expected.buffers.at(buffer.id).draft &&
                   buffer.readMarker == "1970-01-01T00:00:00Z" && buffer.unread == 7);
            assert(buffer.messages.size() == 1 && buffer.title == "Channel A" &&
                   buffer.topic == "local topic");
            auto deleted = storage.history(buffer.id, -1).front();
            deleted.deleted = true;
            storage.update(buffer.id, deleted);
            deleted.deleted = false;
            deleted.text = "must not resurrect";
            storage.append(buffer.id, deleted);
            const auto tombstone = storage.history(buffer.id, -1).front();
            assert(tombstone.deleted && tombstone.text.empty() && tombstone.reactions.empty());
            storage.read(buffer.id, "server-id");
            Model readBack;
            storage.load(readBack);
            assert(readBack.buffers.at(buffer.id).readMarker == "server-id" &&
                   readBack.buffers.at(buffer.id).unread == 0);
            storage.append("Libera/#b", message("other", now + 2));
            storage.clearHistory(buffer.id);
            assert(storage.history(buffer.id).empty() && storage.history("Libera/#b").size() == 1);
            storage.clearHistory();
            Settings settings = actual.settings;
            settings.retentionDays = 1;
            settings.maxMessages = 10;
            storage.prune(settings);
            storage.append(buffer.id, message("expired", now - 2 * 86400));
            storage.append(buffer.id, message("unknown-time", 0));
            const auto retained = storage.history(buffer.id);
            assert(retained.size() == 1 &&
                   retained[0].timestamp == 0); // Unknown server time is never invented.
            storage.clearHistory();
            auto local = message("local:outgoing-check", 0, "locally assigned ID");
            storage.append(buffer.id, local);
            auto anonymousRow = message("", 0, "ID-less local row");
            anonymousRow.own = anonymousRow.pending = true;
            storage.append(buffer.id, anonymousRow);
            const auto localRows = storage.history(buffer.id, -1);
            assert(localRows.size() == 2 && localRows[0].id == local.id &&
                   localRows[1].id.starts_with("local:"));
            assert(storage.message(buffer.id, localRows[1].id)->text == "ID-less local row");
            assert(storage.message(buffer.id, localRows[1].id)->sendStatus == SendStatus::Unknown);
            const auto beforeAnonymous = storage.history(buffer.id, 1, localRows[1].id);
            assert(beforeAnonymous.size() == 1 && beforeAnonymous.front().id == local.id);
            auto anonymous = localRows[1];
            anonymous.deleted = true;
            storage.update(buffer.id, anonymous);
        }
        {
            Storage reopened;
            const auto localRows = reopened.history("Libera/#a", -1);
            assert(localRows.size() == 2 && localRows[0].id == "local:outgoing-check");
            assert(localRows[1].id.starts_with("local:") && localRows[1].deleted &&
                   localRows[1].text.empty());
            assert(localRows[1].reactions.empty() && localRows[1].timestamp == 0);
            assert(reopened.message("Libera/#a", localRows[1].id)->deleted);
            assert(reopened.message("Libera/#a", localRows[1].id)->sendStatus ==
                   SendStatus::Unknown);
            const auto beforeAnonymous = reopened.history("Libera/#a", 1, localRows[1].id);
            assert(beforeAnonymous.size() == 1 && beforeAnonymous.front().id == localRows[0].id);
            reopened.clearHistory();
            Settings pagination;
            pagination.retentionDays = 0;
            pagination.maxMessages = 250;
            reopened.prune(pagination);
            for (int index = 0; index < 201; ++index)
                reopened.append("Libera/#lookup",
                                message("older-" + std::to_string(index), now + index));
            assert(reopened.history("Libera/#lookup").size() == 200);
            auto outsideTail = reopened.message("Libera/#lookup", "older-0");
            assert(outsideTail && outsideTail->id == "older-0");
            outsideTail->reactions["👍"] = {"bob"};
            reopened.update("Libera/#lookup", *outsideTail);
            assert(reopened.message("Libera/#lookup", "older-0")->reactions ==
                   outsideTail->reactions);
            reopened.clearHistory();
            reopened.append("Libera/#lookup", message("closed-but-durable", now));
            Model open;
            reopened.load(open);
            assert(open.buffers.contains("Libera/#lookup"));
            const auto reopenable =
                open.buffers.at("Libera/#lookup"); // This buffer has never had saved metadata.
            open.buffers.erase("Libera/#lookup");
            reopened.save(open);
            Model closed;
            reopened.load(closed);
            assert(!closed.buffers.contains("Libera/#lookup"));
            assert(reopened.message("Libera/#lookup", "closed-but-durable"));
            reopened.append("Libera/#lookup", message("closed-later", now + 1));
            reopened.load(closed);
            assert(!closed.buffers.contains("Libera/#lookup"));
            closed.buffers.emplace(reopenable.id, reopenable);
            reopened.save(closed);
            Model openedAgain;
            reopened.load(openedAgain);
            assert(openedAgain.buffers.contains("Libera/#lookup"));
            reopened.clearHistory();
            Model rename;
            reopened.load(rename);
            rename.settings.maxMessages = 20;
            rename.settings.retentionDays = 0;
            Buffer oldNick;
            oldNick.id = "Libera/Old";
            oldNick.server = "Libera";
            oldNick.target = oldNick.title = "Old";
            oldNick.kind = "query";
            oldNick.draft = "old draft";
            oldNick.readMarker = "unique-old";
            oldNick.unread = 1;
            Buffer newNick = oldNick;
            newNick.id = "Libera/New";
            newNick.target = newNick.title = "New";
            newNick.draft = "new draft";
            newNick.readMarker = "shared";
            newNick.unread = 3;
            rename.buffers.emplace(oldNick.id, oldNick);
            rename.buffers.emplace(newNick.id, newNick);
            rename.activeBuffer = oldNick.id;
            reopened.save(rename);
            auto sharedOld = message("shared", now), sharedNew = message("shared", now);
            sharedOld.reactions["👍"] = {"alice"};
            sharedNew.reactions["👍"] = {"bob"};
            reopened.append(oldNick.id, sharedOld);
            reopened.append(newNick.id, sharedNew);
            auto uniqueOld = message("unique-old", now + 1);
            uniqueOld.reply = "shared";
            reopened.append(oldNick.id, uniqueOld);
            reopened.append(newNick.id, message("unique-new", now + 2));
            auto sourceDeleted = message("redacted-shared", now + 3);
            sourceDeleted.deleted = true;
            auto destinationVisible = message("redacted-shared", now + 3, "must be erased");
            destinationVisible.reactions["👍"] = {"bob"};
            reopened.append(oldNick.id, sourceDeleted);
            reopened.append(newNick.id, destinationVisible);
            reopened.renameBuffer(oldNick.id, newNick.id);
            assert(reopened.history(oldNick.id).empty() &&
                   reopened.history(newNick.id).size() == 4);
            const auto merged = reopened.message(newNick.id, "shared");
            assert(merged && merged->reactions.at("👍").size() == 2);
            const auto redacted = reopened.message(newNick.id, "redacted-shared");
            assert(redacted && redacted->deleted && redacted->text.empty() &&
                   redacted->reactions.empty());
            Model renamed;
            reopened.load(renamed);
            assert(!renamed.buffers.contains(oldNick.id) && renamed.activeBuffer == newNick.id);
            const auto &mergedBuffer = renamed.buffers.at(newNick.id);
            assert(mergedBuffer.draft == string("new draft") + char(10) + "old draft" &&
                   mergedBuffer.readMarker == "unique-old" && mergedBuffer.unread == 2);
            assert(mergedBuffer.target == "New" && mergedBuffer.title == "New" &&
                   mergedBuffer.kind == "query");
            reopened.renameBuffer(newNick.id, "Libera/new");
            reopened.load(renamed);
            assert(renamed.activeBuffer == "Libera/new" &&
                   renamed.buffers.at("Libera/new").target == "new");
            reopened.clearHistory();
        }
        fractionalHistory(data / "state.sqlite3");
        queuedSends(data / "state.sqlite3", now);
        sendBoundaries(data / "state.sqlite3", now);
        writtenSendStatus(data / "state.sqlite3", now);
        activityRoundTrip(now);
        activitySchema6Migration(data / "state.sqlite3", config, now);
        chatFlags(data / "state.sqlite3", config, now);
        chatActivityMetadata(now);
        contentHistoryWindows(now);
        retainedMentions(data / "state.sqlite3", now);
        protocolHistoryStorage(data / "state.sqlite3", now);
        historyPlaybackOrdering(data / "state.sqlite3", now);
        mode(config, 0700);
        mode(data, 0700);
        mode(config / "config.toml", 0600);
        mode(config / "settings.toml", 0600);
        mode(data / "state.sqlite3", 0600);
        // Exercise an actual schema upgrade, preserving the v1 tables.
        dropHistorySchema(data / "state.sqlite3");
        sql(data / "state.sqlite3",
            "DROP TABLE reactions; DROP TABLE closed_buffers; DROP TABLE queued_sends; "
            "ALTER TABLE messages DROP COLUMN activity; "
            "ALTER TABLE messages DROP COLUMN activity_target; "
            "ALTER TABLE buffers DROP COLUMN muted; "
            "ALTER TABLE buffers DROP COLUMN archived; PRAGMA user_version=1;");
        {
            Storage migrated;
            migrated.append("Libera/#migration", message("migrated", now));
        }
        const string originalConfig = contents(config / "config.toml");
        messageBindingPreferences(config);
        conversationBindingPreferences(config);
        presenceInheritancePreferences(config);
        peopleModePreferences(config);
        spellingPreferences(config);
        predictionPreferences(config);
        // Missing spelling defaults never claim a custom shortcut or its chord prefix.
        for (const string chord : {"", "C-;", "C-; a", "c-;"}) {
            for (const string saved : {"missing", "", "M-F6"}) {
                {
                    std::ofstream output(config / "settings.toml");
                    output << "schema_version = 1\n[bindings]\nuser-command = '" << chord << "'\n";
                    if (saved != "missing")
                        output << "cycle-spelling = '" << saved << "'\n";
                }
                for (int reopen = 0; reopen < 2; ++reopen) {
                    Storage storage;
                    Model migrated;
                    storage.load(migrated);
                    assert(migrated.settings.bindings.at("user-command") == chord);
                    assert(migrated.settings.bindings.at("cycle-spelling") ==
                           (saved != "missing" ? saved
                            : chord.empty()    ? "C-;"
                                               : ""));
                    storage.save(migrated);
                }
            }
        }
        // A newly introduced shortcut must not shadow an existing command or chord prefix.
        for (const string chord : {"C-x j", "C-x j a", "C-x"}) {
            {
                std::ofstream output(config / "settings.toml");
                output << "schema_version = 1\n[bindings]\ntoggle-navigation = '" << chord
                       << "'\nuser-command = 'M-F12'\nstyle-modern = 'F6'\n"
                          "style-glass = 'F7'\nstyle-hacker = 'F8'\n";
            }
            const auto checkBindings = [&](const Model &actual) {
                assert(actual.settings.bindings.at("toggle-navigation") == chord);
                assert(actual.settings.bindings.at("jump-channel").empty());
                assert(actual.settings.bindings.at("user-command") == "M-F12");
                for (const char *obsolete : {"style-modern", "style-glass", "style-hacker"})
                    assert(!actual.settings.bindings.contains(obsolete));
            };
            {
                Storage storage;
                Model migrated;
                storage.load(migrated);
                checkBindings(migrated);
                for (const char *obsolete : {"style-modern", "style-glass", "style-hacker"})
                    migrated.settings.bindings[obsolete] = "F12";
                storage.save(migrated);
                const string saved = contents(config / "settings.toml");
                for (const char *obsolete : {"style-modern", "style-glass", "style-hacker"})
                    assert(saved.find(obsolete) == string::npos);
            }
            {
                Storage storage;
                Model reopened;
                storage.load(reopened);
                checkBindings(reopened);
            }
        }
        {
            Storage storage;
            Model configured;
            fs::remove(config / "settings.toml");
            storage.load(configured);
            assert(!configured.firstRun && !fs::exists(config / "settings.toml"));
        }
        // Legacy preferences retain their effect level and acquire independent send defaults.
        {
            std::ofstream output(config / "settings.toml");
            output << "schema_version = 1\n[appearance]\nopacity = 0.5\n"
                      "[workspace]\n1 = 'Libera/#b'\n2 = 'Libera/#a'\nfocused = '2'\n"
                      "visible_buffers = 'Libera/#a|Libera/#b'\nfuture_layout = 'retained'\n"
                      "[bindings]\nsplit-below = 'C-z'\nclose-pane = 'M-x'\n"
                      "'aux-float:people' = 'F9'\n'split-buffer:right' = 'F10'\n"
                      "'reorder-tab:left' = 'F11'\nnext-tab = 'M-F8'\n"
                      "close-tab = 'M-F9'\njump-channel = 'M-j'\n"
                      "user-command = 'M-F12'\n";
        }
        Model legacy;
        {
            Storage storage;
            storage.load(legacy);
            assert(!legacy.firstRun && legacy.settings.opacity == .5f);
            assert(!legacy.settings.transparency && legacy.settings.messageLayouts.empty());
            assert(legacy.settings.channelMessageLayout == MessageLayout::Stacked &&
                   legacy.settings.directMessageLayout == MessageLayout::Bubbles);
            assert(legacy.settings.scale == 1.f && legacy.settings.messageScale == 1.f);
            assert(!legacy.settings.bindings.contains("split-below") &&
                   !legacy.settings.bindings.contains("close-pane") &&
                   !legacy.settings.bindings.contains("aux-float:people") &&
                   !legacy.settings.bindings.contains("split-buffer:right") &&
                   !legacy.settings.bindings.contains("reorder-tab:left") &&
                   !legacy.settings.bindings.contains("next-tab") &&
                   !legacy.settings.bindings.contains("close-tab") &&
                   legacy.settings.bindings.at("jump-channel") == "M-j" &&
                   legacy.settings.bindings.at("user-command") == "M-F12");
            assert(legacy.settings.sendMotion == SendMotion::Flight &&
                   legacy.settings.sendReveal == SendReveal::None);
            assert(legacy.settings.fx == FxLevel::Full && legacy.settings.sound);
            assert(legacy.settings.cursorShape == "line" &&
                   legacy.settings.cursorBlink == "step" && !legacy.settings.cursorTrail &&
                   legacy.settings.cursorTrailStyle == "classic" &&
                   legacy.settings.cursorSmoothBlinkMs == 1500);
            assert(legacy.settings.peopleMode == SidebarMode::Hidden &&
                   legacy.settings.workspace == expected.settings.workspace);
            assert(legacy.identities[0].password == expected.identities[0].password);
        }
        // Obsolete style/material input is ignored, even with invalid values or types.
        for (const string obsolete :
             {"style = 'future-style_2'\n[presentation.glass]\nblur = nan\n"
              "refraction = -0.1\nflow = 2.1\n[presentation.hacker]\nglow = inf\n"
              "scanlines = 'yes'\n",
              "style = false\nglass = 'obsolete'\nhacker = [false]\n",
              "style = ''\n[presentation.glass]\nblur = 'bad'\nrefraction = []\n"
              "flow = false\n[presentation.hacker]\nglow = {}\nscanlines = 3\n"}) {
            {
                std::ofstream output(config / "settings.toml");
                output << "[appearance]\nopacity = 0.5\nui_font = 'Legacy UI font'\n"
                          "message_font = 'Legacy message font'\n"
                          "[workspace]\n1 = 'Libera/#b'\n2 = 'Libera/#a'\nfocused = '2'\n"
                          "visible_buffers = 'Libera/#a|Libera/#b'\nfuture_layout = 'retained'\n"
                          "[presentation]\nirc_channels = ['Libera/#a', 'Other/#a']\n"
                       << obsolete;
            }
            {
                Storage storage;
                storage.load(legacy);
                assert(legacy.settings.peopleMode == SidebarMode::Hidden &&
                       legacy.settings.workspace == expected.settings.workspace);
                assert(legacy.settings.sendMotion == SendMotion::Flight &&
                       legacy.settings.sendReveal == SendReveal::None);
                assert(legacy.settings.channelMessageLayout == MessageLayout::Stacked &&
                       legacy.settings.directMessageLayout == MessageLayout::Bubbles);
                assert(legacy.settings.messageLayouts == expected.settings.messageLayouts &&
                       legacy.settings.uiFont == "Legacy UI font" &&
                       legacy.settings.messageFont == "Legacy message font");
                assert(legacy.settings.uiFontWeight == 400 &&
                       legacy.settings.messageFontWeight == 400);
            }
            auto &s = legacy.settings;
            for (SidebarMode mode :
                 {SidebarMode::Expanded, SidebarMode::Compact, SidebarMode::Hidden}) {
                s.peopleMode = mode;
                {
                    Storage storage;
                    storage.save(legacy);
                }
                Storage reopened;
                Model actual;
                reopened.load(actual);
                presentation(actual.settings, s);
                assert(actual.settings.peopleMode == mode &&
                       actual.settings.workspace == s.workspace &&
                       !actual.settings.workspace.contains("people_visible") &&
                       !actual.settings.workspace.contains("people_mode"));
                assert(actual.settings.opacity == .5f &&
                       contents(config / "config.toml") == originalConfig);
                const string saved = contents(config / "settings.toml");
                assert(saved.find("irc_channels") == string::npos &&
                       saved.find("message_layouts") != string::npos);
                assert(!std::regex_search(
                    saved,
                    std::regex(
                        R"((^|\n)[ \t]*(style|blur|refraction|flow|glow|scanlines)[ \t]*=)")));
                assert(saved.find("glass") == string::npos && saved.find("hacker") == string::npos);
            }
        }
        // New defaults override the old scalar independently; per-buffer layouts still win
        // over the old list.
        struct LayoutCase {
            const char *source;
            MessageLayout channel, direct;
        };
        for (const auto &[source, channel, direct] :
             {LayoutCase{"message_layout = 'bubbles'\n", MessageLayout::Stacked,
                         MessageLayout::Bubbles},
              LayoutCase{"message_layout = 'irc'\n", MessageLayout::Irc, MessageLayout::Irc},
              LayoutCase{"message_layout = 'stacked'\n", MessageLayout::Stacked,
                         MessageLayout::Stacked},
              LayoutCase{"message_layout = 'irc'\nchannel_message_layout = 'bubbles'\n",
                         MessageLayout::Bubbles, MessageLayout::Irc},
              LayoutCase{"message_layout = 'stacked'\ndirect_message_layout = 'irc'\n",
                         MessageLayout::Stacked, MessageLayout::Irc},
              LayoutCase{"channel_message_layout = 'irc'\ndirect_message_layout = 'stacked'\n",
                         MessageLayout::Irc, MessageLayout::Stacked}}) {
            {
                std::ofstream output(config / "settings.toml");
                output << "[presentation]\n" << source
                       << "irc_channels = ['legacy/ignored']\n"
                          "[presentation.message_layouts]\n"
                          "'Libera/#a' = 'stacked'\n'Other/#a' = 'irc'\n'Explicit/#b' = 'bubbles'\n";
            }
            Storage storage;
            Model layouts;
            storage.load(layouts);
            assert(layouts.settings.channelMessageLayout == channel &&
                   layouts.settings.directMessageLayout == direct &&
                   layouts.settings.messageLayouts.size() == 3 &&
                   layouts.settings.messageLayouts.at("Libera/#a") == MessageLayout::Stacked &&
                   layouts.settings.messageLayouts.at("Other/#a") == MessageLayout::Irc &&
                   layouts.settings.messageLayouts.at("Explicit/#b") == MessageLayout::Bubbles &&
                   !layouts.settings.messageLayouts.contains("legacy/ignored"));
            storage.save(layouts);
            const auto saved = toml::parse(contents(config / "settings.toml"));
            const auto *row = saved["presentation"].as_table();
            assert(row && !row->contains("message_layout") && !row->contains("irc_channels") &&
                   row->contains("channel_message_layout") &&
                   row->contains("direct_message_layout") && row->contains("message_layouts"));
            Model reopened;
            storage.load(reopened);
            assert(reopened.settings.channelMessageLayout == channel &&
                   reopened.settings.directMessageLayout == direct &&
                   reopened.settings.messageLayouts == layouts.settings.messageLayouts);
        }
        // Every motion/reveal pairing is independently decoded and survives save/restart.
        for (const auto &[motionToken, motion] :
             vector<std::pair<string, SendMotion>>{{"none", SendMotion::None},
                                                   {"flight", SendMotion::Flight},
                                                   {"rise", SendMotion::Rise},
                                                   {"pop", SendMotion::Pop},
                                                   {"slide", SendMotion::Slide}}) {
            for (const auto &[revealToken, reveal] : vector<std::pair<string, SendReveal>>{
                     {"none", SendReveal::None}, {"fade", SendReveal::Fade}}) {
                {
                    std::ofstream output(config / "settings.toml");
                    output << "[appearance]\nsend_motion = '" << motionToken << "'\nsend_reveal = '"
                           << revealToken << "'\n";
                }
                Storage storage;
                Model configured;
                storage.load(configured);
                assert(configured.settings.sendMotion == motion &&
                       configured.settings.sendReveal == reveal);
                storage.save(configured);
                Model reopened;
                storage.load(reopened);
                presentation(reopened.settings, configured.settings);
                assert(contents(config / "config.toml") == originalConfig);
            }
        }
        // Removed effects are accepted on load, then saved as canonical values in both directions.
        {
            const auto file = config / "settings.toml";
            const string source = "[appearance]\nsend_motion = 'drift'\nsend_reveal = 'pixels'\n"
                                  "incoming_motion = 'drift'\nincoming_reveal = 'pixels'\n";
            {
                std::ofstream output(file);
                output << source;
            }
            Storage storage;
            Model migrated;
            storage.load(migrated);
            assert(contents(file) == source);
            assert(migrated.settings.sendMotion == SendMotion::Rise &&
                   migrated.settings.sendReveal == SendReveal::None &&
                   migrated.settings.incomingMotion == SendMotion::Rise &&
                   migrated.settings.incomingReveal == SendReveal::None);
            storage.save(migrated);
            const auto saved = toml::parse_file(file.string());
            assert(saved["appearance"]["send_motion"].value<string>() == "rise" &&
                   saved["appearance"]["send_reveal"].value<string>() == "none" &&
                   saved["appearance"]["incoming_motion"].value<string>() == "rise" &&
                   saved["appearance"]["incoming_reveal"].value<string>() == "none");
            Model reopened;
            storage.load(reopened);
            presentation(reopened.settings, migrated.settings);
            assert(contents(config / "config.toml") == originalConfig);
        }
        for (const char *obsolete : {"mention_style = 'tint_only'\nmention_arrival = true\n",
                                     "mention_effect = 'luminous_outline'\nmention_strength = 0.75\n"
                                     "mention_speed = 1.5\nmention_outline_breathing = true\n"
                                     "mention_underline_glint = false\n"}) {
            const auto file = config / "settings.toml";
            const string source = "[appearance]\n"
                                  "send_motion = 'slide'\nsend_reveal = 'none'\n"
                                  "incoming_motion = 'pop'\nincoming_reveal = 'fade'\n"
                                  "incoming_duration_ms = 1250\n" +
                                  string(obsolete);
            {
                std::ofstream output(file);
                output << source;
            }
            Storage storage;
            Model configured;
            storage.load(configured);
            assert(contents(file) == source);
            assert(configured.settings.sendMotion == SendMotion::Slide &&
                   configured.settings.sendReveal == SendReveal::None &&
                   configured.settings.incomingMotion == SendMotion::Pop &&
                   configured.settings.incomingReveal == SendReveal::Fade &&
                   configured.settings.incomingDurationMs == 1250.f);
            storage.save(configured);
            const auto saved = toml::parse_file(file.string());
            const auto appearance = saved["appearance"];
            for (const char *key : {"mention_style", "mention_arrival", "mention_effect",
                                    "mention_strength", "mention_speed",
                                    "mention_outline_breathing", "mention_underline_glint"})
                assert(!appearance[key]);
            Model reopened;
            storage.load(reopened);
            presentation(reopened.settings, configured.settings);
            assert(contents(config / "config.toml") == originalConfig);
        }
        // Either field may be absent without changing the explicitly configured other field.
        for (bool motionOnly : {true, false}) {
            const string source =
                motionOnly ? "[appearance]\nsend_motion = 'none'\nincoming_motion = 'none'\n"
                           : "[appearance]\nsend_reveal = 'fade'\nincoming_reveal = 'fade'\n";
            {
                std::ofstream output(config / "settings.toml");
                output << source;
            }
            Storage storage;
            Model actual;
            storage.load(actual);
            const auto reveal = motionOnly ? SendReveal::None : SendReveal::Fade;
            assert(actual.settings.sendMotion ==
                       (motionOnly ? SendMotion::None : SendMotion::Flight) &&
                   actual.settings.incomingMotion ==
                       (motionOnly ? SendMotion::None : SendMotion::Pop));
            assert(actual.settings.sendReveal == reveal &&
                   actual.settings.incomingReveal == reveal);
            assert(actual.settings.incomingDurationMs == 280.f &&
                   actual.settings.foolDisplay == FoolDisplay::Minimized &&
                   !actual.settings.foolUnread && actual.settings.messageWrapColumns == 80);
            assert(contents(config / "settings.toml") == source);
        }
        for (bool ui : {true, false})
            for (int weight : {300, 400, 500, 600, 700}) {
                const string key = ui ? "ui_font_weight" : "message_font_weight";
                {
                    std::ofstream output(config / "settings.toml");
                    output << "[appearance]\n" << key << " = " << weight << '\n';
                }
                Storage storage;
                Model configured;
                storage.load(configured);
                assert(configured.settings.uiFontWeight == (ui ? weight : 400) &&
                       configured.settings.messageFontWeight == (ui ? 400 : weight));
                storage.save(configured);
                Model reopened;
                storage.load(reopened);
                presentation(reopened.settings, configured.settings);
                assert(contents(config / "config.toml") == originalConfig);
            }
        // New absent fields preserve defaults; all supported policies round-trip independently.
        for (int style = 0; style < 4; ++style) {
            for (const auto motion :
                 {SendMotion::None, SendMotion::Rise, SendMotion::Pop, SendMotion::Slide}) {
                Model configured = legacy;
                auto &s = configured.settings;
                s.linkPreviews = PreviewMode(style % 3);
                s.imagePreviews = PreviewMode((style + 1) % 3);
                s.mediaPreviews = PreviewMode((style + 2) % 3);
                s.previewMaxHeight = style == 0 ? 64 : 240;
                s.previewMediaMaxMiB = style == 0 ? 1 : 512;
                s.previewMediaMaxSeconds = style == 0 ? 1 : 7200;
                s.previewAllowLocal = style % 2 != 0;
                s.foolDisplay = FoolDisplay(style);
                s.foolUnread = style % 2 != 0;
                s.messageWrapColumns = style == 0 ? 0 : style == 3 ? 1000 : 80;
                s.incomingMotion = motion;
                s.incomingReveal = SendReveal(style % 2);
                s.incomingDurationMs = style == 0 ? 100.f : style == 3 ? 2000.f : 550.f;
                s.messageScale = style == 0 ? .7f : style == 3 ? 2.f : 1.3f;
                Storage storage;
                storage.save(configured);
                Model reopened;
                storage.load(reopened);
                presentation(reopened.settings, s);
                assert(contents(config / "config.toml") == originalConfig);
            }
        }
        for (const string source : {"fool_display = 'unknown'",
                                    "ui_font_weight = 299",
                                    "ui_font_weight = 401",
                                    "ui_font_weight = 800",
                                    "ui_font_weight = 400.0",
                                    "ui_font_weight = '400'",
                                    "ui_font_weight = true",
                                    "ui_font_weight = nan",
                                    "ui_font_weight = inf",
                                    "ui_font_weight = []",
                                    "message_font_weight = 0",
                                    "message_font_weight = 650",
                                    "message_font_weight = 701",
                                    "message_font_weight = 400.0",
                                    "message_font_weight = '400'",
                                    "message_font_weight = false",
                                    "message_font_weight = nan",
                                    "message_font_weight = -inf",
                                    "message_font_weight = {}",
                                    "fool_display = 1",
                                    "fool_unread = 1",
                                    "fool_unread = 'false'",
                                    "message_wrap_columns = -1",
                                    "message_wrap_columns = 1001",
                                    "message_wrap_columns = 80.0",
                                    "message_wrap_columns = true",
                                    "incoming_motion = 'flight'",
                                    "incoming_motion = 'unknown'",
                                    "incoming_reveal = 'unknown'",
                                    "incoming_reveal = false",
                                    "incoming_duration_ms = 99",
                                    "incoming_duration_ms = 2001",
                                    "incoming_duration_ms = nan",
                                    "incoming_duration_ms = inf",
                                    "incoming_duration_ms = '550'",
                                    "message_scale = 0.699",
                                    "message_scale = 2.001",
                                    "message_scale = nan",
                                    "message_scale = inf",
                                    "message_scale = -inf",
                                    "message_scale = '1.2'",
                                    "message_scale = true"}) {
            const string invalidSource = "[appearance]\n" + source + "\n";
            {
                std::ofstream output(config / "settings.toml");
                output << invalidSource;
            }
            Storage storage;
            Model retained;
            retained.settings.messageWrapColumns = 17;
            bool failed = false;
            try {
                storage.load(retained);
            } catch (const std::runtime_error &) {
                failed = true;
            }
            assert(failed && contents(config / "settings.toml") == invalidSource &&
                   contents(config / "config.toml") == originalConfig);
        }
        for (const string source :
             {"links = 'unknown'", "images = false", "media = 1", "max_height = 63",
              "max_height = 241", "max_height = 120.0", "media_max_mib = 0", "media_max_mib = 513",
              "media_max_mib = '64'", "media_max_seconds = 0", "media_max_seconds = 7201",
              "media_max_seconds = true", "allow_local = 1", "allow_local = 'true'"}) {
            const string invalidSource = "[previews]\n" + source + "\n";
            {
                std::ofstream output(config / "settings.toml");
                output << invalidSource;
            }
            Storage storage;
            Model configured;
            bool failed = false;
            try {
                storage.load(configured);
            } catch (const std::runtime_error &) {
                failed = true;
            }
            assert(failed && contents(config / "settings.toml") == invalidSource &&
                   contents(config / "config.toml") == originalConfig);
        }
        // Effects policy and all cursor preferences survive restart.
        const FxLevel levels[] = {FxLevel::Off, FxLevel::Subtle, FxLevel::Full};
        const char *shapes[] = {"line", "box", "underline"};
        const char *blinks[] = {"off", "step", "smooth"};
        const int fades[] = {200, 333, 500, 625, 1000, 5000};
        const char *trailStyles[] = {"classic", "motion-blur", "blaze", "lightning",
                                     "railgun", "torpedo", "pixiedust"};
        for (std::size_t index = 0; index < 2 * std::size(trailStyles); ++index) {
            Model configured = legacy;
            auto &s = configured.settings;
            s.fx = levels[index % 3];
            s.cursorShape = shapes[index % 3];
            s.cursorBlink = blinks[index % 3];
            s.cursorSmoothBlinkMs = fades[index % std::size(fades)];
            s.cursorTrail = index >= std::size(trailStyles);
            s.cursorTrailStyle = trailStyles[index % std::size(trailStyles)];
            s.reducedMotion = true;
            s.sound = true;
            Storage storage;
            storage.save(configured);
            const string saved = contents(config / "settings.toml");
            const auto document = toml::parse(saved);
            assert(document["cursor"]["trail_style"].value<string>() == s.cursorTrailStyle);
            assert(document["cursor"]["smooth_blink_ms"].value_exact<int64_t>() ==
                   s.cursorSmoothBlinkMs);
            Model actual;
            storage.load(actual);
            assert(actual.settings.fx == s.fx && actual.settings.cursorShape == s.cursorShape &&
                   actual.settings.cursorBlink == s.cursorBlink &&
                   actual.settings.cursorSmoothBlinkMs == s.cursorSmoothBlinkMs &&
                   actual.settings.cursorTrail == s.cursorTrail &&
                   actual.settings.cursorTrailStyle == s.cursorTrailStyle);
            assert(actual.settings.reducedMotion && actual.settings.sound &&
                   contents(config / "settings.toml") == saved &&
                   contents(config / "config.toml") == originalConfig);
        }
        // Legacy movement modes migrate to Trail unless an explicit Trail key is present.
        struct CursorCase {
            const char *source;
            const char *blink;
            bool trail;
        };
        for (const auto &test :
             {CursorCase{"", "step", false},
              CursorCase{"effect = 'off'\n", "step", false},
              CursorCase{"effect = 'trail'\n", "step", true},
              CursorCase{"effect = 'comet'\n", "step", true},
              CursorCase{"effect = 'ribbon'\n", "step", true},
              CursorCase{"effect = 'pulse'\n", "step", true},
              CursorCase{"effect = 'comet'\nblink = 'off'\n", "off", true},
              CursorCase{"effect = 'ribbon'\nblink = 'smooth'\ntrail = false\n", "smooth", false},
              CursorCase{"effect = 'off'\ntrail = true\n", "step", true},
              CursorCase{"effect = false\ntrail = true\n", "step", true}}) {
            const auto file = config / "settings.toml";
            const string source =
                "[cursor]\nshape = 'underline'\ntrail_duration_ms = 'unused'\n"
                "trail_strength = [0.2]\n" +
                string(test.source);
            std::ofstream(file) << source;
            Storage storage;
            Model actual;
            storage.load(actual);
            assert(actual.settings.cursorShape == "underline" &&
                   actual.settings.cursorBlink == test.blink &&
                   actual.settings.cursorSmoothBlinkMs == 1500 &&
                   actual.settings.cursorTrail == test.trail &&
                   actual.settings.cursorTrailStyle == "classic" && contents(file) == source);
            storage.save(actual);
            const auto saved = toml::parse_file(file.string());
            const auto cursor = saved["cursor"];
            assert(cursor["shape"].value<string>() == "underline" &&
                   cursor["blink"].value<string>() == test.blink &&
                   cursor["smooth_blink_ms"].value_exact<int64_t>() == 1500 &&
                   cursor["trail"].value<bool>() == test.trail &&
                   cursor["trail_style"].value<string>() == "classic" &&
                   !cursor["effect"] && !cursor["trail_duration_ms"] && !cursor["trail_strength"]);
            Model reopened;
            storage.load(reopened);
            assert(reopened.settings.cursorShape == "underline" &&
                   reopened.settings.cursorBlink == test.blink &&
                   reopened.settings.cursorSmoothBlinkMs == 1500 &&
                   reopened.settings.cursorTrail == test.trail &&
                   reopened.settings.cursorTrailStyle == "classic" &&
                   contents(config / "config.toml") == originalConfig);
        }
        // Invalid in-memory preferences must fail before either configuration file is replaced.
        const string validSettings = contents(config / "settings.toml");
        for (bool ui : {true, false})
            for (int weight : {-1, 0, 299, 350, 401, 650, 701, 800}) {
                Storage storage;
                Model invalid = legacy;
                (ui ? invalid.settings.uiFontWeight : invalid.settings.messageFontWeight) = weight;
                bool failed = false;
                try {
                    storage.save(invalid);
                } catch (const std::runtime_error &error) {
                    failed = string(error.what())
                                 .find(ui ? "appearance.ui_font_weight"
                                          : "appearance.message_font_weight") != string::npos;
                }
                assert(failed && contents(config / "config.toml") == originalConfig &&
                       contents(config / "settings.toml") == validSettings);
            }
        for (float scale :
             {.69f, 2.01f, std::numeric_limits<float>::infinity(),
              -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
            Storage storage;
            Model invalid = legacy;
            invalid.settings.messageScale = scale;
            bool failed = false;
            try {
                storage.save(invalid);
            } catch (const std::runtime_error &error) {
                failed = string(error.what()).find("appearance.message_scale") != string::npos;
            }
            assert(failed && contents(config / "config.toml") == originalConfig &&
                   contents(config / "settings.toml") == validSettings);
        }
        for (const string diagnostic :
             {"appearance.send_motion", "appearance.send_reveal", "appearance.incoming_motion",
              "appearance.incoming_duration_ms", "appearance.fool_display",
              "appearance.message_wrap_columns", "previews",
              "previews.max_height", "previews.media_max_mib", "previews.media_max_seconds",
              "presentation.channel_message_layout", "presentation.direct_message_layout",
              "presentation.message_layouts.Libera/#a",
              "cursor.shape", "cursor.blink", "cursor.trail_style", "cursor.smooth_blink_ms"}) {
            Storage storage;
            Model invalid = legacy;
            if (diagnostic == "cursor.shape")
                invalid.settings.cursorShape = "block";
            else if (diagnostic == "cursor.blink")
                invalid.settings.cursorBlink = "particles";
            else if (diagnostic == "cursor.trail_style")
                invalid.settings.cursorTrailStyle = "motion_blur";
            else if (diagnostic == "cursor.smooth_blink_ms")
                invalid.settings.cursorSmoothBlinkMs = 5001;
            else if (diagnostic == "appearance.send_motion")
                invalid.settings.sendMotion = static_cast<SendMotion>(-1);
            else if (diagnostic == "appearance.incoming_motion")
                invalid.settings.incomingMotion = SendMotion::Flight;
            else if (diagnostic == "appearance.incoming_duration_ms")
                invalid.settings.incomingDurationMs = std::numeric_limits<float>::quiet_NaN();
            else if (diagnostic == "appearance.fool_display")
                invalid.settings.foolDisplay = static_cast<FoolDisplay>(-1);
            else if (diagnostic == "appearance.message_wrap_columns")
                invalid.settings.messageWrapColumns = 1001;
            else if (diagnostic == "previews")
                invalid.settings.linkPreviews = static_cast<PreviewMode>(-1);
            else if (diagnostic == "previews.max_height")
                invalid.settings.previewMaxHeight = 63;
            else if (diagnostic == "previews.media_max_mib")
                invalid.settings.previewMediaMaxMiB = 513;
            else if (diagnostic == "previews.media_max_seconds")
                invalid.settings.previewMediaMaxSeconds = 7201;
            else if (diagnostic == "presentation.channel_message_layout")
                invalid.settings.channelMessageLayout = static_cast<MessageLayout>(-1);
            else if (diagnostic == "presentation.direct_message_layout")
                invalid.settings.directMessageLayout = static_cast<MessageLayout>(-1);
            else if (diagnostic == "presentation.message_layouts.Libera/#a")
                invalid.settings.messageLayouts["Libera/#a"] = static_cast<MessageLayout>(-1);
            else
                invalid.settings.sendReveal = static_cast<SendReveal>(-1);
            bool failed = false;
            try {
                storage.save(invalid);
            } catch (const std::runtime_error &error) {
                failed = string(error.what()).find(diagnostic) != string::npos;
            }
            assert(failed && contents(config / "config.toml") == originalConfig &&
                   contents(config / "settings.toml") == validSettings);
        }
        {
            Storage storage;
            Model invalid = legacy;
            invalid.settings.foolDisplay = FoolDisplay::Shown;
            bool failed = false;
            try {
                storage.save(invalid);
            } catch (const std::runtime_error &error) {
                failed = string(error.what()).find("appearance.fool_display") != string::npos;
            }
            assert(failed && contents(config / "config.toml") == originalConfig &&
                   contents(config / "settings.toml") == validSettings);
        }
        // Native profile helpers and direct saves must reject the same invalid model.
        // Change preferences too, so a premature settings write cannot pass unnoticed.
        auto rejectProfile = [&](Model invalid, bool invalidIdentity) {
            invalid.settings.opacity = .42f;
            assert(validateIdentityProfile(invalid, 0).empty() == !invalidIdentity);
            assert(!validateServerProfile(invalid, 0).empty());
            Storage storage;
            bool failed = false;
            try {
                storage.save(invalid);
            } catch (const std::runtime_error &) {
                failed = true;
            }
            assert(failed && contents(config / "config.toml") == originalConfig &&
                   contents(config / "settings.toml") == validSettings);
        };
        for (const auto &[field, value] : vector<std::pair<string Identity::*, string>>{
                 {&Identity::name, ""},
                 {&Identity::nick, "#bad"},
                 {&Identity::nick, ""},
                 {&Identity::nick, "bad\rnick"},
                 {&Identity::nick, "bad\nnick"},
                 {&Identity::nick, string("bad\0nick", 8)},
                 {&Identity::nick, string(506, 'n')},
                 {&Identity::username, ""},
                 {&Identity::username, ":bad"},
                 {&Identity::username, "bad user"},
                 {&Identity::username, "bad\ruser"},
                 {&Identity::username, "bad\nuser"},
                 {&Identity::username, string("bad\0user", 8)},
                 {&Identity::username, string(513, 'u')},
                 {&Identity::realname, "bad\rname"},
                 {&Identity::realname, "bad\nname"},
                 {&Identity::realname, string("bad\0name", 8)},
                 // USER has ten fixed bytes; this produces a 511-byte frame.
                 {&Identity::realname, string(501 - legacy.identities[0].username.size(), 'r')},
                 {&Identity::realname,
                  " " + string(499 - legacy.identities[0].username.size(), 'r')},
                 {&Identity::password, string(16385, 'p')},
                 {&Identity::password, string("bad\0password", 12)},
                 {&Identity::certificate, string("/tmp/bad\0.pem", 13)},
                 {&Identity::key, string("/tmp/bad\0.key", 13)}}) {
            Model invalid = legacy;
            invalid.identities[0].*field = value;
            rejectProfile(invalid, true);
        }
        for (const auto &[field, value] : vector<std::pair<string ServerConfig::*, string>>{
                 {&ServerConfig::name, ""},
                 {&ServerConfig::identity, "missing identity"},
                 {&ServerConfig::host, ""},
                 {&ServerConfig::host, "bad host"},
                 {&ServerConfig::host, "bad\rhost"},
                 {&ServerConfig::host, string("bad\0host", 8)},
                 {&ServerConfig::host, string(254, 'h')},
                 {&ServerConfig::sasl, "plain"},
                 {&ServerConfig::sasl, "UNKNOWN"},
                 {&ServerConfig::password, "bad\rpassword"},
                 {&ServerConfig::password, "bad\npassword"},
                 {&ServerConfig::password, string("bad\0password", 12)},
                 {&ServerConfig::password, string(506, 'p')},
                 // A space requires PASS's extra trailing-parameter colon.
                 {&ServerConfig::password, " " + string(504, 'p')},
                 {&ServerConfig::bouncer, "unknown"},
                 {&ServerConfig::bouncer, "soju:"},
                 {&ServerConfig::bouncer, "soju:bad id"},
                 {&ServerConfig::bouncer, "soju:" + string(498, 'b')}}) {
            Model invalid = legacy;
            invalid.servers[0].*field = value;
            rejectProfile(invalid, false);
        }
        for (int port : {0, 65536}) {
            Model invalid = legacy;
            invalid.servers[0].port = port;
            rejectProfile(invalid, false);
        }
        for (const string mechanism : {"PLAIN", "EXTERNAL"}) {
            Model invalid = legacy;
            invalid.servers[0].sasl = mechanism;
            invalid.servers[0].tls = false;
            rejectProfile(invalid, false);
        }
        {
            Model invalid = legacy;
            invalid.servers[0].sasl = "EXTERNAL";
            invalid.identities[0].certificate.clear();
            rejectProfile(invalid, false);
        }
        for (const string &channel :
             vector<string>{"#bad\rchannel", "#bad\nchannel", string("#bad\0channel", 12)}) {
            Model invalid = legacy;
            invalid.servers[0].channels = {channel};
            rejectProfile(invalid, false);
        }
        {
            Model invalid = legacy;
            invalid.identities.push_back(invalid.identities[0]);
            assert(!validateIdentityProfile(invalid, 1).empty());
            rejectProfile(invalid, true);
        }
        {
            Model invalid = legacy;
            invalid.servers.push_back(invalid.servers[0]);
            assert(!validateServerProfile(invalid, 1).empty());
            rejectProfile(invalid, false);
        }
        auto roundTripProfile = [&](const Model &configured) {
            assert(validateIdentityProfile(configured, 0).empty() &&
                   validateServerProfile(configured, 0).empty());
            {
                Storage storage;
                storage.save(configured);
            }
            Storage reopened;
            Model actual;
            reopened.load(actual);
            assert(!actual.firstRun && actual.identities.size() == 1 && actual.servers.size() == 1);
            const auto &a = actual.identities[0];
            const auto &e = configured.identities[0];
            assert(a.name == e.name && a.nick == e.nick && a.username == e.username &&
                   a.realname == e.realname && a.password == e.password &&
                   a.certificate == e.certificate && a.key == e.key);
            const auto &s = actual.servers[0];
            const auto &t = configured.servers[0];
            assert(s.name == t.name && s.host == t.host && s.port == t.port && s.tls == t.tls &&
                   s.identity == t.identity && s.sasl == t.sasl && s.password == t.password &&
                   s.bouncer == t.bouncer && s.channels == t.channels);
            assert(validateIdentityProfile(actual, 0).empty() &&
                   validateServerProfile(actual, 0).empty());
        };
        {
            Model valid = legacy;
            valid.identities[0].nick = "Éclair猫";
            valid.identities[0].username = "équipe/phone";
            valid.identities[0].realname = "Display name with spaces";
            valid.identities[0].password = "SASL password with spaces\r\nsecond line";
            valid.identities[0].certificate = "/tmp/combined client.pem";
            valid.identities[0].key.clear(); // EXTERNAL accepts a combined certificate/key PEM.
            valid.servers[0].sasl = "EXTERNAL";
            valid.servers[0].host = "2001:db8::1";
            valid.servers[0].bouncer = "soju:network/phone";
            roundTripProfile(valid);

            // Inclusive wire/field bounds: NICK 505, USER/PASS 510, SASL 16 KiB,
            // address 253, soju binding 497, and both ends of the port range.
            valid.identities[0].nick = string(505, 'n');
            valid.identities[0].username = string(250, 'u');
            valid.identities[0].realname = string(250, 'r');
            valid.identities[0].password = string(16384, 'p');
            valid.servers[0].password = string(505, 'p');
            valid.servers[0].host = string(253, 'h');
            valid.servers[0].bouncer = "soju:" + string(497, 'b');
            valid.servers[0].port = 1;
            roundTripProfile(valid);
            valid.identities[0].realname = " " + string(248, 'r');
            valid.servers[0].password = " " + string(503, 'p');
            valid.servers[0].port = 65535;
            roundTripProfile(valid);
            valid.identities[0].username = string(499, 'u');
            valid.identities[0].realname.clear(); // Empty real names also use a trailing colon.
            valid.servers[0].sasl.clear();
            valid.servers[0].tls = false;
            roundTripProfile(valid);
        }
        // Restore the exact fixture, including preferences, for the remaining failure loops.
        {
            std::ofstream profiles(config / "config.toml");
            profiles << originalConfig;
            std::ofstream preferences(config / "settings.toml");
            preferences << validSettings;
        }
        // A valid first row followed by an invalid profile must not partially replace Model.
        for (const string &source : vector<string>{
                 "[[identity]]\nname = 'first'\nnick = 'valid'\nusername = 'valid'\n"
                 "[[identity]]\nname = 'second'\nnick = '#bad'\nusername = 'valid'\n",
                 "[[identity]]\nname = 'first'\nnick = 'valid'\nusername = 'valid'\n"
                 "[[identity]]\nname = 'second'\nnick = 'valid'\nusername = 'bad user'\n",
                 "[[identity]]\nname = 'first'\nnick = 'valid'\nusername = 'valid'\n"
                 "[[server]]\nname = 'network'\naddress = '2001:db8::1'\n"
                 "identity = 'first'\nsasl = 'PLAIN'\ntls = false\n"}) {
            {
                std::ofstream output(config / "config.toml");
                output << source;
            }
            Storage storage;
            Model untouched = legacy;
            untouched.firstRun = true;
            untouched.activeBuffer = "sentinel";
            untouched.identities[0].nick = "sentinel";
            untouched.servers[0].host = "sentinel.invalid";
            untouched.settings.opacity = .37f;
            untouched.buffers["sentinel/buffer"] = sendBuffer("sentinel/buffer");
            untouched.buffers.at("sentinel/buffer").draft = "sentinel draft";
            bool failed = false;
            try {
                storage.load(untouched);
            } catch (const std::runtime_error &) {
                failed = true;
            }
            assert(failed && untouched.firstRun && untouched.activeBuffer == "sentinel" &&
                   untouched.identities.size() == legacy.identities.size() &&
                   untouched.identities[0].name == legacy.identities[0].name &&
                   untouched.identities[0].nick == "sentinel" &&
                   untouched.servers.size() == legacy.servers.size() &&
                   untouched.servers[0].host == "sentinel.invalid" &&
                   untouched.buffers.size() == legacy.buffers.size() + 1 &&
                   untouched.buffers.at("sentinel/buffer").draft == "sentinel draft" &&
                   untouched.settings.opacity == .37f);
            presentation(untouched.settings, legacy.settings);
            for (bool corrected : {false, true}) {
                if (corrected) {
                    std::ofstream output(config / "config.toml");
                    output << originalConfig;
                }
                failed = false;
                try {
                    storage.save(legacy);
                } catch (const std::runtime_error &) {
                    failed = true;
                }
                assert(failed &&
                       contents(config / "config.toml") == (corrected ? originalConfig : source) &&
                       contents(config / "settings.toml") == validSettings);
            }
            Model corrected;
            storage.load(corrected); // Repair alone is insufficient; a fresh load clears refusal.
            assert(!corrected.firstRun &&
                   corrected.identities[0].nick == legacy.identities[0].nick &&
                   corrected.identities[0].password == legacy.identities[0].password);
            storage.save(corrected);
            assert(contents(config / "config.toml") == originalConfig &&
                   contents(config / "settings.toml") == validSettings);
        }
        // Parse/type/range failures leave both the caller's state and source files untouched.
        const vector<std::pair<string, string>> invalidCases = {
            {"[appearance\ninvalid TOML", "cannot parse configuration"},
            {"[appearance]\nfx = 'maximum'\n", "appearance.fx"},
            {"[appearance]\nfx = false\n", "fx"},
            {"[presence]\nmode = 'global'\n", "presence.mode"},
            {"[appearance]\ntransparency = 'yes'\n", "transparency"},
            {"cursor = 'line'\n", "cursor"},
            {"[cursor]\nshape = 'block'\n", "cursor.shape"},
            {"[cursor]\neffect = 'particles'\n", "cursor.effect"},
            {"[cursor]\neffect = 'Trail'\n", "cursor.effect"},
            {"[cursor]\neffect = ''\n", "cursor.effect"},
            {"[cursor]\nshape = 1\n", "shape"},
            {"[cursor]\neffect = false\n", "effect"},
            {"[cursor]\nblink = 'fade'\n", "cursor.blink"},
            {"[cursor]\nblink = 'Smooth'\n", "cursor.blink"},
            {"[cursor]\nblink = ''\n", "cursor.blink"},
            {"[cursor]\nblink = true\n", "blink"},
            {"[cursor]\nsmooth_blink_ms = 199\n", "cursor.smooth_blink_ms"},
            {"[cursor]\nsmooth_blink_ms = 5001\n", "cursor.smooth_blink_ms"},
            {"[cursor]\nsmooth_blink_ms = 500.0\n", "smooth_blink_ms"},
            {"[cursor]\nsmooth_blink_ms = '500'\n", "smooth_blink_ms"},
            {"[cursor]\nsmooth_blink_ms = true\n", "smooth_blink_ms"},
            {"[cursor]\ntrail = 'true'\n", "trail"},
            {"[cursor]\neffect = 'trail'\ntrail = 1\n", "trail"},
            {"[cursor]\ntrail_style = 'off'\n", "cursor.trail_style"},
            {"[cursor]\ntrail_style = 'Motion Blur'\n", "cursor.trail_style"},
            {"[cursor]\ntrail_style = ''\n", "cursor.trail_style"},
            {"[cursor]\ntrail_style = false\n", "trail_style"},
            {"[appearance]\nsend_motion = 'fly'\n", "appearance.send_motion"},
            {"[appearance]\nsend_motion = 'Flight'\n", "appearance.send_motion"},
            {"[appearance]\nsend_motion = ''\n", "appearance.send_motion"},
            {"[appearance]\nsend_motion = 1\n", "send_motion"},
            {"[appearance]\nsend_reveal = 'pixel'\n", "appearance.send_reveal"},
            {"[appearance]\nsend_reveal = 'Pixels'\n", "appearance.send_reveal"},
            {"[appearance]\nsend_reveal = ''\n", "appearance.send_reveal"},
            {"[appearance]\nsend_reveal = false\n", "send_reveal"},
            {"[presentation]\nirc_channels = 'Libera/#a'\n", "presentation.irc_channels"},
            {"[presentation]\nirc_channels = ['Libera/#a', false]\n", "presentation.irc_channels"},
            {"[presentation]\nmessage_layout = false\n", "presentation.message_layout"},
            {"[presentation]\nmessage_layout = 'IRC'\n", "presentation.message_layout"},
            {"[presentation]\nmessage_layout = ''\n", "presentation.message_layout"},
            {"[presentation]\nmessage_layout = 'other'\n", "presentation.message_layout"},
            {"[presentation]\nchannel_message_layout = false\n", "presentation.channel_message_layout"},
            {"[presentation]\nchannel_message_layout = 'IRC'\n", "presentation.channel_message_layout"},
            {"[presentation]\ndirect_message_layout = false\n", "presentation.direct_message_layout"},
            {"[presentation]\ndirect_message_layout = 'IRC'\n", "presentation.direct_message_layout"},
            {"[presentation]\nmessage_layouts = []\n", "message_layouts"},
            {"[presentation.message_layouts]\n'a' = false\n", "presentation.message_layouts.a"},
            {"[presentation.message_layouts]\n'a' = 'IRC'\n", "presentation.message_layouts.a"},
            {"[presentation.message_layouts]\n'a' = ''\n", "presentation.message_layouts.a"},
            {"[presentation.message_layouts]\n'a' = 'other'\n", "presentation.message_layouts.a"},
            {"[appearance]\nopacity = 0.25\n[workspace]\npeople_visible = 'true'\n",
             "people_visible"},
            {"[workspace]\npeople_visible = 1\n", "people_visible"},
            {"[workspace]\npeople_mode = 'unknown'\npeople_visible = true\n",
             "workspace.people_mode"},
            {"[workspace]\npeople_mode = true\npeople_visible = true\n", "workspace.people_mode"},
            {"[workspace]\npeople_mode = 1\npeople_visible = true\n", "workspace.people_mode"},
            {"[workspace]\npeople_mode = 0.0\npeople_visible = true\n", "workspace.people_mode"},
            {"[workspace]\npeople_mode = ['expanded']\npeople_visible = true\n",
             "workspace.people_mode"},
            {"[workspace]\npeople_mode = { value = 'expanded' }\npeople_visible = true\n",
             "workspace.people_mode"},
            {"[workspace]\npeople_visible = 0.0\n", "people_visible"},
            {"[workspace]\npeople_visible = [true]\n", "people_visible"},
            {"[workspace]\npeople_visible = { enabled = true }\n", "people_visible"},
            {"[workspace]\nfuture_layout = true\n", "workspace.future_layout"},
            {"[workspace]\n2 = false\n", "workspace.2"}};
        for (const auto &[source, diagnostic] : invalidCases) {
            {
                std::ofstream output(config / "settings.toml");
                output << source;
            }
            Storage storage;
            Model untouched;
            untouched.activeBuffer = "sentinel";
            untouched.settings = expected.settings;
            untouched.settings.cursorTrailStyle = "pixiedust";
            bool failed = false;
            try {
                storage.load(untouched);
            } catch (const std::runtime_error &error) {
                failed = string(error.what()).find(diagnostic) != string::npos;
            }
            assert(failed && untouched.activeBuffer == "sentinel");
            presentation(untouched.settings, expected.settings);
            assert(untouched.settings.fx == expected.settings.fx &&
                   untouched.settings.cursorShape == expected.settings.cursorShape &&
                   untouched.settings.cursorBlink == expected.settings.cursorBlink &&
                   untouched.settings.cursorTrail == expected.settings.cursorTrail &&
                   untouched.settings.cursorTrailStyle == "pixiedust");
            assert(untouched.settings.peopleMode == expected.settings.peopleMode &&
                   untouched.settings.workspace == expected.settings.workspace &&
                   untouched.settings.opacity == expected.settings.opacity);
            failed = false;
            try {
                storage.save(expected);
            } catch (const std::runtime_error &) {
                failed = true;
            }
            assert(failed && contents(config / "config.toml") == originalConfig &&
                   contents(config / "settings.toml") == source);
        }
        {
            std::ofstream(config / "config.toml") << originalConfig;
            std::ofstream(config / "settings.toml") << validSettings;
        }
        {
            Storage storage;
            Model configured;
            storage.load(configured);
            auto &upload = configured.settings.uploads;
            upload.provider = UploadProvider::Custom;
            upload.customEndpoint = "https://private.example:8443/upload";
            upload.expiryHours = "";
            upload.litterboxExpiry = "72h";
            upload.secret = false;
            upload.maxBytes = 17ULL * 1024 * 1024;
            upload.username = "uploader";
            upload.password = "never-include-this-secret-in-errors";
            ServerConfig parent = configured.servers.front();
            parent.name = "Account stable key";
            parent.bouncer = "soju";
            parent.sojuAccount = true;
            parent.sojuTrustedFileHost = "files.example";
            ServerConfig child = parent;
            child.name = "Legacy display selector";
            child.sojuAccount = false;
            child.sojuParent = parent.name;
            child.sojuNetworkId = "42";
            child.bouncer = "soju:42";
            child.sojuEnabled = false;
            child.password.clear();
            child.host.clear();
            child.identity.clear();
            child.sojuTrustedFileHost.clear();
            configured.servers.push_back(parent);
            configured.servers.push_back(child);
            const string retained = child.name + "/#retained";
            configured.buffers[retained] = sendBuffer(retained);
            configured.buffers[retained].draft = "retained draft";
            configured.buffers[retained].readMarker = "retained-read";
            configured.settings.workspace["soju-group:" + parent.name] = "collapsed";
            storage.save(configured);
            Model reopened;
            storage.load(reopened);
            const auto &actual = reopened.settings.uploads;
            assert(actual.provider == upload.provider &&
                   actual.customEndpoint == upload.customEndpoint && actual.expiryHours.empty() &&
                   actual.litterboxExpiry == "72h" && !actual.secret &&
                   actual.maxBytes == upload.maxBytes && actual.username == upload.username &&
                   actual.password == upload.password);
            assert(reopened.servers[reopened.servers.size() - 2].sojuAccount);
            const auto &restored = reopened.servers.back();
            assert(restored.name == child.name && restored.sojuParent == parent.name &&
                   restored.sojuNetworkId == "42" && !restored.sojuEnabled &&
                   restored.password.empty());
            assert(reopened.buffers.at(retained).draft == "retained draft" &&
                   reopened.buffers.at(retained).readMarker == "retained-read" &&
                   reopened.settings.workspace.at("soju-group:" + parent.name) == "collapsed");
            reopened.settings.timestamps = !reopened.settings.timestamps;
            storage.save(reopened);
            storage.load(reopened);
            assert(reopened.settings.uploads.password == upload.password);
            mode(config / "config.toml", 0600);
            mode(config / "settings.toml", 0600);
        }
        const string uploadSettings = contents(config / "settings.toml");
        const string sojuProfiles = contents(config / "config.toml");
        for (const string source :
             {"uploads = false\n", "[uploads]\nprovider = 'unknown'\n",
              "[uploads]\nprovider = false\n", "[uploads]\nsecret = 'true'\n",
              "[uploads]\nsecret = 0\n", "[uploads]\nsize_limit_mib = true\n",
              "[uploads]\nsize_limit_mib = 25.0\n", "[uploads]\nsize_limit_mib = 0\n",
              "[uploads]\nsize_limit_mib = 1.5\n", "[uploads]\nexpiry_hours = 'tomorrow'\n",
              "[uploads]\nlitterbox_expiry = 'forever'\n",
              "[uploads]\ncustom_endpoint = 'http://unsafe.example'\n",
              "[uploads]\npassword = false\n"}) {
            std::ofstream(config / "settings.toml") << source;
            Storage storage;
            Model untouched;
            untouched.settings.uploads.password = "sentinel-secret";
            bool failed = false;
            try {
                storage.load(untouched);
            } catch (const std::runtime_error &error) {
                const string diagnostic = error.what();
                failed = true;
                assert(diagnostic.find("sentinel-secret") == string::npos &&
                       diagnostic.find("never-include-this-secret-in-errors") == string::npos);
            }
            assert(failed && untouched.settings.uploads.password == "sentinel-secret" &&
                   contents(config / "settings.toml") == source &&
                   contents(config / "config.toml") == sojuProfiles);
        }
        std::ofstream(config / "settings.toml") << uploadSettings;
        {
            Storage storage;
            Model duplicate;
            storage.load(duplicate);
            auto copy = duplicate.servers.back();
            copy.name = "Other local key same remote";
            duplicate.servers.push_back(copy);
            bool failed = false;
            try {
                storage.save(duplicate);
            } catch (const std::runtime_error &) {
                failed = true;
            }
            assert(failed && contents(config / "config.toml") == sojuProfiles &&
                   contents(config / "settings.toml") == uploadSettings);
        }
        {
            Storage storage;
            Model invalidParent;
            storage.load(invalidParent);
            const auto parent =
                std::find_if(invalidParent.servers.begin(), invalidParent.servers.end(),
                             [](const auto &entry) { return entry.sojuAccount; });
            assert(parent != invalidParent.servers.end());
            const auto identity =
                std::find_if(invalidParent.identities.begin(), invalidParent.identities.end(),
                             [&](const auto &entry) { return entry.name == parent->identity; });
            assert(identity != invalidParent.identities.end());
            identity->username = "account/display-selector";
            assert(!validateServerProfile(invalidParent,
                                          std::size_t(parent - invalidParent.servers.begin()))
                        .empty());
            bool failed = false;
            try {
                storage.save(invalidParent);
            } catch (const std::runtime_error &) {
                failed = true;
            }
            assert(failed && identity->username == "account/display-selector" &&
                   contents(config / "config.toml") == sojuProfiles &&
                   contents(config / "settings.toml") == uploadSettings);
        }
        for (const string extra :
             {"[[server]]\nname = 'Bad role type'\nsoju_account = 1\n",
              "[[server]]\nname = 'Bad enabled type'\nsoju_enabled = 0\n",
              "[[server]]\nname = 'Missing parent'\nbouncer = 'soju:x'\n"
              "soju_parent = 'Absent account'\nsoju_network_id = 'x'\n",
              "[[server]]\nname = 'Partial association'\nsoju_parent = 'Account stable key'\n",
              "[[server]]\nname = 'Bad trusted host'\nsoju_trusted_file_host = "
              "'https://evil.example'\n"}) {
            std::ofstream(config / "config.toml") << sojuProfiles << extra;
            Storage storage;
            Model untouched;
            untouched.status = "unchanged";
            bool failed = false;
            try {
                storage.load(untouched);
            } catch (const std::runtime_error &) {
                failed = true;
            }
            assert(failed && untouched.status == "unchanged" &&
                   contents(config / "config.toml") == sojuProfiles + extra &&
                   contents(config / "settings.toml") == uploadSettings);
        }
        localDataScopes(root / "local-data-scopes", now);
        fs::remove_all(root);
        std::cout << "storage round-trip, retention, migrations, redaction, echo correlation, "
                     "queued-send handoff and presentation/workspace validation and parse "
                     "protection passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << "\nRetained check directory: " << root << '\n';
        return 1;
    }
}
