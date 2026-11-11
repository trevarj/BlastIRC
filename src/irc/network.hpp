#pragma once
#include "../model.hpp"
#include <memory>

namespace blast {
// All public methods are thread-safe. start/stop are idempotent; stop wakes DNS,
// connect, TLS, reads and reconnect waits. poll transfers logical events in source
// order across interleaved/nested multiline batches; control, sent and own ghosts are
// delivered immediately, rather than waiting for an unrelated open batch.
// Enqueues throw runtime_error at 4096 requests/8 MiB or while stopped/between
// connections; receiving applies bounded backpressure (16 MiB/32768 events).
// Pre-start enqueue is allowed. poll regularly; stop interrupts receive waits.
// Application owns JOIN intent on Ready. bouncer="soju:<id>" restores a binding.
// A rejected BIND is fatal: no Ready on an unselected network and no automatic retry.
// Events retain original IRC tags/params; server is ServerConfig::name. time and
// timestamp come ONLY from a valid server `time` tag (zero/empty otherwise).
// history marks chathistory, ZNC playback and nested historical event batches.
// message/action/notice: target conversation, nick sender, text body, id msgid,
// reply +reply/+draft/reply, label inherited through labeled-response batches.
// Own sends get tags[pending]=1 and a local id/label; echoed events share the label.
// A caller submission labels its first ghost and .part1, .part2, ... descendants.
// Each carries tags[submission]; only the last carries tags[submission-complete]=1
// and tags[submission-parts] (total logical ghosts), after all earlier ghosts were
// emitted. This is a handoff, not delivery ACK.
// A ghost containing synthetic reply-prefix bytes also carries original-text
// with those bytes removed (possibly empty); text always matches its wire body.
// sent: own=true, label matches an admitted ghost, emitted only after its entire
// CRLF frame is written (closing BATCH for multiline, once per logical ghost).
// This is local transport completion, not a server acknowledgement or echo.
// tags[echo-expected] is "1"/"0", captured at frame admission, not completion.
// state: text Connecting/Registering/Ready/Disconnected/Failed; tags[error].
// Ready follows network-selection updates and acknowledged deferred capabilities.
// error: text reason. Failed sends additionally carry target, attempt label when
// a ghost exists (or caller submission before it), tags[send-failed]=1 and
// tags[original-text] for draft recovery; /me recovery text retains its command.
// No retries on uncertain delivery. Only explicit multiline rejection retries
// once as legacy chunks, retaining the first attempt label without a false error.
// caps: params enabled cap NAMES, tags cap name -> advertised value.
// network: tags[kind] is isupport/bouncer/metadata/monitor/rename/standard-reply/
// invite/account/chghost/setname/relaymsg/search/netsplit/netjoin/latency/raw;
// command in tags[command]. ISUPPORT tags are a complete current snapshot,
// including CASEMAPPING/CHANTYPES/PREFIX; absent tokens have been removed.
// CLIENTTAGDENY is delivered in ISUPPORT, not a fictitious reply/react capability.
// Metadata: METADATA/761/766 use kind=metadata and target the literal target
// (params[0] for METADATA, params[1] after the numeric's recipient for 761/766).
// Params remain untouched: missing METADATA value and 766 mean deletion; an
// explicit empty METADATA/761 value is retained. text is the value, not 766 prose.
// draft/metadata-2 requires batch. On Ready or later enable, SUB avatar precedes
// SYNC of live joined channels, preserving pre-Ready 774 deadlines; own JOIN also
// synchronizes. If subscriptions are unavailable (max-subs=0), GET avatar replaces
// SUB/SYNC and 774 is ignored. Subscribed 774 retries wait at least RetryAfter
// seconds (5 if absent), coalesce per joined channel, and follow live RENAME/CASEMAPPING.
// PART/KICK/disconnect or loss of either cap cancel retries; cap loss also purges
// metadata held behind multiline batches. Updates require both caps currently active.
// rename: target old channel, params[1] new channel, text optional reason;
// live renames retarget pending echo/reaction/history/typing/metadata bookkeeping.
// latency.text is milliseconds. Search messages retain target and tags[search]=1.
// Bouncer rows: id network id, text name, tags attrs; deleted rows tags[deleted]=1.
// Catalog: network kind=bouncer-list, id=batch ID, tags[phase]=begin/end (also
// emitted for empty batches). Only sojuAccount control sessions request catalogs.
// sojuMutation queues ADDNETWORK/CHANGENETWORK/DELNETWORK; requires Ready and
// bouncer-networks on the control session. One in flight; never automatically retried.
// network kind=bouncer-ack/fail: tags[command] mutation, id netid when known.
// Independent LISTNETWORKS failures also use bouncer-fail, without consuming a mutation.
// Failure text is safe fixed prose; echoed attributes/credentials are not retained.
// authentication: network tags[authenticated]=1 and tags[mechanism] on SASL 903.
// names: params individually contain prefix+nick[!user@host]; tags[complete]=1 at
// end of NAMES; tags[PREFIX] gives `(modes)prefixes`. join: account/realname tags.
// part/quit: text reason; kick: nick actor, params[1] victim; nick: nick old,
// text new; topic: text topic. away: nick, text reason, tags[away]=0/1.
// typing: text active/paused/done (self/history typing filtered). reaction:
// id referenced msgid, text emoji; caller supplies explicit add/removal intent.
// tags[remove]=1 for removals. redact: id deleted msgid, text optional reason.
// Failed optimistic reactions reverse with tags[rollback]=1 (no draft mutation).
// typing(target,"disabled") cancels local timers without sending a tag. Send done
// explicitly only when privacy settings allow it; send() never bypasses settings.
// read: text unprefixed RFC3339 marker, private to one logical IRC identity across
// its connections; sharing an account alone need not attach different nicknames.
// history_begin/end: target, label caller requestId, tags[initial]=1 for initial sync,
// tags[batch], tags[direction]=latest/before/after/around/search, tags[limit] page size.
// End tags[count] counts primary logical events, including event-playback
// presence, not auxiliary draft/chathistory-context records. tags[cursor] is
// the last primary event's MSGREFTYPES-supported msgid=/timestamp= selector.
// End carries tags[error] on rejection/timeout/disconnect; tags[rejected]=1 means
// no protocol retry (admission/unsupported/disconnect). Deliberately canceled
// pending initial requests also carry tags[cancelled]=1, with no generic error.
// Historical NICK/QUIT use their batch conversation target, never mutate membership.
// Events without server time deliberately do not invent historical timestamps.
// history(target,before,requestId,initial) accepts opaque msgids, msgid= or timestamp= selectors;
// bare ids are never interpreted as dates. An after= prefix requests gap-fill
// AFTER. Outgoing times keep milliseconds. Callers with cached rows resolve ids
// to their original server time when MSGREFTYPES permits only timestamps.
// Public enqueue throws synchronously when stopped/reconnecting or its submission
// queue is full; no request was admitted and no history_end follows that exception.
// command accepts raw IRC (optional leading slash) plus /me, /msg, /query,
// /search <target> <query>, /bouncer <network-id>. Invalid/injected/oversized lines
// report error events. commandId is opaque and independent of submission.
// Correlated replies retain their normal state semantics and add command-id,
// command-name (original query), and command-response (numeric plus original
// parameters excluding recipient). Ambiguous WHOIS replies carry command-name
// only: consumers may suppress diagnostics but must not attach profile fields.
// command-complete=1 retires the route; ACK/batch close/expiry/disconnect use
// network kind=command-complete with no response text. Unlabeled MODE/TOPIC primary
// 324/332 release the active query, retaining origin for optional 329/333 for five
// seconds only when unambiguous. Expiry, invalidation and disconnect retire that
// nonblocking context silently. Unresolved idless ambiguity markers remain bounded
// until reconnect; they never block primary replies, only uncertain optional replies.
// Correlation is bounded to 128 requests and 30 seconds. Text sends split UTF-8
// safely; negotiated multiline is
// atomic and respects advertised max-lines/max-bytes. Draft operations are gated
// by negotiated caps and CLIENTTAGDENY. Outbound tags use +reply and +draft/react
// (the latter emoji, with +reply carrying its msgid); deployed aliases are read.
// react(...,removing) selects +draft/unreact for removal, +draft/react otherwise;
// cap/CLIENTTAGDENY gates apply to the selected tag and +reply.
// send() retains raw text for retries; fallbackNick prefixes it exactly once when
// a wire reply id is absent or +reply is not currently permitted.
// CLIENTTAGDENY uses irc::clientTagAllowed: case-sensitive wildcard rules, with
// matching '-' exceptions overriding denials; message-tags must also be enabled.
// TLS chain and hostname verification are mandatory; encrypted client keys fail
// noninteractively. STS is advertisement-only (never CAP REQ); verified policies
// persist and their last positive duration is renewed on connection close.
class NetworkClient {
  public:
    NetworkClient(const ServerConfig &, const Identity &);
    ~NetworkClient();
    NetworkClient(const NetworkClient &) = delete;
    NetworkClient &operator=(const NetworkClient &) = delete;
    void start();
    void stop();
    void send(const string &target, const string &text, const string &reply = "",
              const string &submission = "", const string &fallbackNick = "");
    void command(const string &line, const string &submission = "", const string &commandId = "");
    void sojuMutation(const string &command, const string &id,
                      const std::map<string, string> &attributes);
    void history(const string &target, const string &before, const string &requestId, bool initial);
    // AROUND uses the first server-supported selector available from msgid and serverTime.
    void historyAround(const string &target, const string &msgid, const string &serverTime,
                       const string &requestId);
    void search(const string &target, const string &query, const string &requestId);
    // First target is viewed; foreground work follows, then remaining ordered initial targets.
    // Only pending work is reprioritized or canceled; sent requests always complete.
    void prioritizeHistory(const vector<string> &targets);
    void cancelInitialHistory(const string &target);
    void markRead(const string &target, const string &timestamp);
    void typing(const string &target, const string &state);
    void react(const string &target, const string &id, const string &emoji, bool removing);
    void redact(const string &target, const string &id);
    vector<Event> poll();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace blast
