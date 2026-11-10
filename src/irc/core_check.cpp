// c++ -std=c++20 -Wall -Wextra -pedantic src/irc/protocol.cpp src/irc/core_check.cpp -o
// /tmp/blastirc-core-check
#include "protocol.hpp"

#include <cassert>
#include <chrono>
#include <stdexcept>
#include <string>

using namespace blast::irc;

namespace {

void rejects(const WireMessage &message, std::size_t coreLimit = 510) {
    bool rejected = false;
    try {
        (void)serialize(message, coreLimit);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    assert(rejected);
}

} // namespace

int main() {
    const std::string escaped = "space ;slash\\\r\n";
    assert(unescapeTag(escapeTag(escaped)) == escaped);
    assert(escapeTag(" ;\r\n\\") == "\\s\\:\\r\\n\\\\");
    assert(unescapeTag("\\unknown\\") == "unknown");

    assert(clientTagAllowed("", "+draft/react"));
    assert(!clientTagAllowed("draft/react", "+draft/react"));
    assert(clientTagAllowed("draft/react", "+reply"));
    assert(!clientTagAllowed("*", "+reply"));
    assert(!clientTagAllowed("draft/*", "+draft/react"));
    assert(!clientTagAllowed("d*ft/*act", "+draft/react"));
    assert(clientTagAllowed("draft/*,-draft/react", "+draft/react"));
    assert(!clientTagAllowed("draft/*,-draft/react", "+draft/unreact"));
    assert(clientTagAllowed("*,-draft/*,-reply", "+draft/react"));
    assert(clientTagAllowed("*,-draft/*,-reply", "+reply"));
    assert(!clientTagAllowed("*,-draft/*,-reply", "+typing"));
    assert(clientTagAllowed("-draft/r*,draft/*", "draft/react"));
    assert(!clientTagAllowed("draft/*,-DRAFT/*", "+draft/react"));

    WireMessage original{{{"+draft/reply", "abc"}, {"example.org/unknown", escaped}, {"flag", ""}},
                         "nick!user@host.example",
                         "PRIVMSG",
                         {"#room", ":hello world"}};
    const auto decoded = parse(serialize(original));
    assert(decoded);
    assert(decoded->tags == original.tags);
    assert(decoded->prefix == original.prefix);
    assert(decoded->command == original.command);
    assert(decoded->params == original.params);
    const auto duplicate = parse("@x=first;x=last;flag=;other :irc.example.org 001 nick :Welcome");
    assert(duplicate && duplicate->tags.at("x") == "last");
    assert(duplicate->tags.at("flag").empty() && duplicate->tags.at("other").empty());
    assert(duplicate->prefix == "irc.example.org");
    const auto futureTags =
        parse("@bad_key=first;bad/key/again=value;;bad_key=last;+ :Other PRIVMSG #room :hello");
    assert(futureTags && futureTags->params.back() == "hello");
    assert(futureTags->tags.at("bad_key") == "last" &&
           futureTags->tags.at("bad/key/again") == "value" && futureTags->tags.contains("+"));
    assert(!futureTags->tags.contains(""));
    rejects(*futureTags); // Only inbound keys are opaque; we emit the registered key syntax.
    assert(parse(":2001:db8::1 NOTICE * :server punctuation"));
    assert(parse("ping  one   :two  three ")->command == "PING");
    assert(parse("PING  one   :two  three ")->params[1] == "two  three ");
    assert(parse("PING :")->params.front().empty());
    assert(serialize(WireMessage{{}, {}, "ping", {""}}) == "PING :");
    assert(serialize(WireMessage{{}, {}, "PING", {":token"}}) == "PING ::token");
    assert(serialize(WireMessage{{}, {}, "PING", {"one two"}}) == "PING :one two");
    assert(serialize(WireMessage{{}, {}, "PING", {"a:b"}}) == "PING a:b");

    for (const auto *invalid :
         {"", " PING", "@ PING", "@x", ": PING", ":server", "12 x", "1234 x",
          "PRI1VMSG #room :text", "PING\r\n", "PING :x\nPRIVMSG #room :injected",
          "PRIVMSG :#room bad", "PRIVMSG :", "PRIVMSG #a,,#b :text"}) {
        assert(!parse(invalid));
    }
    assert(!parse(std::string("PING :a\0b", 9)));
    assert(!parse(std::string("@x=a\0b PING", 11)));
    assert(parse("123 nick :numeric"));
    assert(parse("@+typing=active TAGMSG #room"));

    WireMessage bad{{}, {}, "PRIVMSG", {"#room", "text\r\nQUIT"}};
    rejects(bad);
    bad.params = {"#room bad", "text"};
    rejects(bad);
    bad.params = {":#room"};
    rejects(bad);
    bad.params = {"#room", std::string("a\0b", 3)};
    rejects(bad);
    bad.params = {"#room", "text"};
    bad.command = "PRIVMSG QUIT";
    rejects(bad);
    bad.command = "PRIVMSG";
    bad.prefix = "nick\nQUIT";
    rejects(bad);
    bad.prefix.clear();
    bad.tags = {{"bad key", "value"}};
    rejects(bad);
    bad.tags = {{"key", std::string("a\0b", 3)}};
    rejects(bad);
    rejects(WireMessage{{}, {}, "TEST", {"", "last"}});
    rejects(WireMessage{{}, {}, "TEST", {":not-middle", "last"}});

    WireMessage many{{}, {}, "TEST", std::vector<std::string>(15, "x")};
    assert(parse(serialize(many))->params.size() == 15);
    many.params.push_back("x");
    rejects(many);
    assert(!parse("TEST a b c d e f g h i j k l m n o p"));
    assert(!parse("TEST a b c d e f g h i j k l m n o :p"));

    // Both limits count bytes independently; tags include '@' and final space.
    const std::string core = "PING :" + std::string(504, 'x');
    assert(core.size() == 510 && parse(core));
    assert(!parse(core + "x"));
    WireMessage fullCore{{}, {}, "PING", {std::string(503, 'x') + " "}};
    assert(serialize(fullCore).size() == 510);
    fullCore.params.front() += 'x';
    rejects(fullCore);
    assert(parse(core + "x", 511));
    assert(serialize(fullCore, 511).size() == 511);
    assert(parse(serialize(fullCore, 511), 511));
    assert(!parse(core, 509) && !parse(core, 65535));
    rejects(fullCore, 509);
    rejects(fullCore, 65535);
    const WireMessage largest{{}, {}, "PING", {std::string(65527, 'x') + " "}};
    assert(serialize(largest, 65534).size() == 65534);
    assert(parse(serialize(largest, 65534), 65534));
    assert(!parse(serialize(largest, 65534) + "x", incomingCoreLimit));
    const std::string incomingTags = "@x=" + std::string(8187, 'x') + " ";
    assert(incomingTags.size() == 8191 && parse(incomingTags + core));
    assert(!parse("@x=" + std::string(8188, 'x') + " PING"));
    assert(!parse(incomingTags + core + "x"));

    // Captured over verified TLS: soju sends this CAP LS before any LINELEN.
    const std::string sojuCapabilities =
        ":localhost CAP * LS :soju.im/bouncer-networks-notify echo-message server-time "
        "message-tags multi-prefix draft/message-redaction batch draft/extended-monitor "
        "account-tag extended-join draft/read-marker draft/pre-away draft/chathistory "
        "labeled-response soju.im/bouncer-networks soju.im/webpush draft/extended-isupport "
        "away-notify soju.im/read no-implicit-names soju.im/search account-notify chghost "
        "invite-notify setname draft/metadata-2=before-connect,max-keys=0,max-value-bytes=1 "
        "draft/no-implicit-names extended-monitor soju.im/no-implicit-names "
        "soju.im/account-required sasl=PLAIN cap-notify";
    assert(sojuCapabilities.size() == 593);
    assert(!parse(sojuCapabilities));
    const auto capabilities = parse(sojuCapabilities, incomingCoreLimit);
    assert(capabilities && capabilities->command == "CAP");
    assert(capabilities->params.size() == 3 && capabilities->params[1] == "LS");
    assert(capabilities->params.back().ends_with("sasl=PLAIN cap-notify"));
    rejects(*capabilities); // Receiving longer frames does not widen outbound limits.
    assert(parse(incomingTags + sojuCapabilities, incomingCoreLimit));
    assert(!parse("@x=" + std::string(8188, 'x') + " " + sojuCapabilities, incomingCoreLimit));

    const auto emptyBody = parse(incomingTags + "PRIVMSG #room :");
    assert(emptyBody && emptyBody->params.back().empty());
    const std::size_t retained = retainedBytes(*emptyBody);
    assert(retained >= 8191);
    constexpr std::size_t batchBudget = 8 * 1024 * 1024;
    std::size_t used = 0, records = 0;
    while (records < 4096 && retained <= batchBudget - used) {
        used += retained;
        ++records;
    }
    assert(records > 0 && records < 4096);
    assert(used <= batchBudget && retained > batchBudget - used);
    WireMessage tagged{{{"x", std::string(4092, 'x')}}, {}, "PING", {}};
    assert(serialize(tagged).size() == 4096 + 4);
    assert(parse(serialize(tagged))->tags == tagged.tags);
    tagged.tags.at("x") += 'x';
    rejects(tagged);
    tagged.tags.at("x") = std::string(2046, ' ');
    assert(serialize(tagged).size() == 4096 + 4);
    tagged.tags.at("x") += ' ';
    rejects(tagged);

    assert(casefold("AZ[]\\^~") == "az{}|~~");
    assert(casefold("AZ[]\\^~", "strict-rfc1459") == "az{}|^~");
    assert(casefold("AZ[]\\^~", "ascii") == "az[]\\^~");
    assert(casefold("AZ[]\\^~", "unknown") == "az{}|~~");
    assert(casefold("AZ[]\\^~", "") == "az{}|~~");
    assert(casefold("\xc3\x84") == "\xc3\x84");
    for (const auto *nick : {"Nick[]\\`_^{}|", "élise", "猫🙂", "Привет", "9nick"})
        assert(validNickname(nick));
    for (const auto *nick :
         {"", "#bad", "&bad", "$bad", "bad,nick", "bad:nick", "a*b", "a?b", "a!b", "a@b"})
        assert(!validNickname(nick));
    for (int control = 0; control <= 32; ++control)
        assert(!validNickname("a" + std::string(1, static_cast<char>(control)) + "b"));
    assert(!validNickname(std::string("a\x7f"
                                      "b",
                                      3)));
    const std::string longestNick(505, 'n');
    assert(validNickname(longestNick) && !validNickname(longestNick + "n"));
    assert(serialize(WireMessage{{}, {}, "NICK", {longestNick}}).size() == 510);
    assert(prefixNick("nick!user@host") == "nick");
    assert(prefixNick("nick@host") == "nick");
    assert(prefixNick("irc.example.org") == "irc.example.org");
    assert(prefixNick("").empty());

    assert(parseTimestamp("1970-01-01T00:00:00Z") == 0);
    assert(parseTimestamp("1969-12-31T23:59:59.999Z") == -1);
    assert(parseTimestamp("1970-01-01T01:30:00.123456789+01:30") == 0);
    assert(parseTimestamp("1969-12-31T22:30:00-01:30") == 0);
    assert(parseTimestamp("1970-01-01t00:00:00z") == 0);
    assert(parseTimestamp("2000-02-29T12:34:56Z"));
    assert(parseTimestamp("2024-02-29T00:00:00Z"));
    assert(parseTimestamp("0001-01-01T00:00:00Z"));
    const auto precise = parseTimestampParts("2026-11-05T01:30:00.1234567899+01:30");
    const auto utc = parseTimestampParts("2026-11-05T00:00:00.123456789Z");
    assert(precise && utc && precise->seconds == utc->seconds &&
           precise->nanoseconds == 123456789 && utc->nanoseconds == precise->nanoseconds);
    assert(parseTimestampParts("2026-11-05T00:00:00.1Z")->nanoseconds == 100000000);
    assert(parseTimestampParts("2026-11-05T00:00:00.000Z")->nanoseconds == 0);
    assert(parseTimestampParts("9999-12-31T23:59:59.999999999Z")->nanoseconds == 999999999);
    for (const auto *invalid :
         {"", "2023-02-29T00:00:00Z", "1900-02-29T00:00:00Z", "2024-04-31T00:00:00Z",
          "2024-00-01T00:00:00Z", "2024-13-01T00:00:00Z", "2024-01-00T00:00:00Z",
          "0000-01-01T00:00:00Z", "2024-01-01T24:00:00Z", "2024-01-01T00:60:00Z",
          "2024-01-01T00:00:60Z", "2024-01-01T00:00:00", "2024-01-01T00:00:00.Z",
          "2024-01-01T00:00:00.1", "2024-01-01T00:00:00Zjunk", "2024-01-01T00:00:00+24:00",
          "2024-01-01T00:00:00+00:60", "2024-01-01T00:00:00+0000", "202x-01-01T00:00:00Z"}) {
        assert(!parseTimestamp(invalid));
        assert(!parseTimestampParts(invalid));
    }
    const auto before = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now())
                            .time_since_epoch()
                            .count();
    const std::string now = timestampNow();
    const auto after = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now())
                           .time_since_epoch()
                           .count();
    assert(now.size() == 20 && now.back() == 'Z');
    const auto timestamp = parseTimestamp(now);
    assert(timestamp && *timestamp >= before && *timestamp <= after);
}
