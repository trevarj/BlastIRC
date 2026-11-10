<p align="center">
  <img src="assets/icon.png" alt="BLAST IRC: an orange and cream rounded-square bomb/chat icon with a terminal prompt and spark" width="240">
</p>

<h1 align="center">BLAST IRC</h1>

A native Linux IRCv3 client with a flat messenger layout, avatar navigation, and keyboard-first editing.

## Features

- **One conversation, less chrome.** A server-avatar rail, expandable channel sidebar, optional right member list, and a concise status bar. No tabs, splits, docking, floating panels, or drag-and-drop layout system.
- **Two ways to read channels.** Telegram-style incoming/outgoing bubbles by default; traditional aligned IRC rows can be selected independently for busy channels.
- **Keyboard-first editing.** Buffer switching, command palette, nickname completion, undo, and familiar Emacs-style bindings.
- **IRCv3 conversations.** Typing indicators, replies, emoji reactions, read markers, and server history when supported by the server or bouncer.
- **Direct or bouncer connections.** TLS, SASL PLAIN and EXTERNAL, ZNC, and soju accounts with independently attached child networks and native network management.
- **Local history.** Search saved messages, load older history, and configure retention. Desktop notifications flag mentions while unfocused and use the current BLAST IRC logo from the canonical installed/development asset through the standard notification image-path hint. IRC notification bodies display literal text, not active markup or remotely loaded images.
- **Your desktop palette.** Live Noctalia template colors, rounded controls, adjustable fonts and scale, and explicitly optional transparency.
- **Optional motion and sound.** Effects, cue controls, and reduced motion.
- **Uploads and previews.** Review file, text, or clipboard uploads before inserting a link; compact link/image/media previews load on click by default.

BLAST IRC is under active development. The Nix package targets **x86_64 Linux with a Wayland desktop**. X11 and Xwayland are not supported.

The application is available under the [MIT license](LICENSE).

## Getting started

### Run with Nix

```sh
nix run path:.
```

Or build and run separately:

```sh
nix build path:.#default
./result/bin/blastirc
```

Run from a Wayland desktop session. The first build downloads pinned dependencies and compiles the client.

### Join your first channel

1. Fill in the welcome screen: server address, port, nickname, and identity.
2. Keep TLS enabled and use your network's authentication details where required.
3. Enter auto-join channels, separated by commas, then choose **Save and connect**.
4. Select a conversation. **Enter** sends and **Shift+Enter** inserts a newline. **Tab** opens nickname or command suggestions; Up/Down selects and Tab/Enter accepts without sending. Escape dismisses unchanged text.

Use **Settings → Networks** to add direct connections or select ZNC mode. **Add soju account** creates a control connection using the bouncer endpoint and a base-account Identity, without a legacy `/network` username suffix. Networks reference shared Identity profiles; editing an Identity affects every network using it.

### Soju accounts

A soju account discovers its networks and attaches each locally enabled child using a separate connection to the same bouncer. New children start enabled. The parent and child avatars share a capsule in the server rail; click the parent to fold or expand it without changing the current conversation. A folded capsule retains aggregate unread and active-child indicators.

On the parent account's **Settings → Networks** page, choose **Password (SASL PLAIN)** and enter **Account username** and **Account password** together with the server address, port and TLS setting. New accounts default to password authentication. These fields edit the selected shared Identity, not a separate IRC Server password; the page lists every profile affected by those edits. Use **Save and connect** or **Save and reconnect** to apply them. **Connect automatically** controls local startup behavior.

The same page shows live account and child connection states and errors. Saving settings is not proof of a successful connection, and changing credentials does not update an already-running session until reconnection.

Discovered children use native `BOUNCER BIND`, which requires pre-registration SASL authentication. **Client certificate (EXTERNAL)** uses the selected Identity's certificate and key through **Edit identity**. The advanced **Management only (no SASL)** mode can use its separate IRC PASS credential for the control connection, but cannot bind child networks. Ordinary IRC profiles retain their separate Server password control.

Set the account's **Account alias** in Settings → Networks, or right-click its avatar and choose **Edit profile…**. This editable display name updates the rail and network picker without changing the internal account key, child connections, history or fold state.

Right-click the account for management, or use **Settings → Networks → Manage soju account**. Add, edit, and remove upstream networks through soju's advertised management protocol. A pending command is not reported as successful until soju acknowledges it. Removing an upstream network requires confirmation and retains local conversations, history, drafts, and reading state.

Disabling a child's **Attach this network locally** control disconnects only BlastIRC's attachment, not soju's upstream connection. Disconnecting the account stops its local child sessions. Existing bound profiles keep their local identities and history when adopted; ambiguous legacy associations require an explicit account/network selection rather than a guessed merge.

### Familiar IRC commands

| Command | Action |
| --- | --- |
| `/join #channel` | Join a channel |
| `/part #channel` | Leave a channel |
| `/msg nickname hello` | Send a private message |
| `/me waves` | Send an action |
| `/nick newname` | Change nickname |
| `/whois nickname` | Open a read-only user profile |
| `/away Back soon` | Set away; `/away` clears it |

Slash suggestions include descriptions from the shared command catalog: client-handled commands and common commands sent to the IRC server are distinguished. Inline ghost text shows only missing required parameters; it is not part of the document, copied text, or sent command. Completion inserts text only on acceptance. Server-command availability and syntax still depend on the server.

Command responses retain their originating conversation and source-network label when correlation is available, even after switching conversations. WHOIS is the exception: `/whois` and message/member Whois actions open a read-only profile with a generated avatar fallback, grouped known fields, loading/error feedback, readable idle duration, and sign-on time in the local timezone. Other commands retain buffer routing. Labeled responses provide the strongest association. Unlabeled replies are associated conservatively. Uncorrelated raw reply output uses the actual source network's server log rather than being guessed into the active conversation; structured replies can still update their normal state or destinations without producing a log entry.

Message actions use one icon-labelled native context menu with grouped submenus, not repeated hover toolbars. Right-click a message for supported reply, reaction, copy, and sender Whois actions. Pointer menus open beside the invocation point; Menu/Shift+F10 uses the focused message instead. Clicking outside closes the menu without activating the underlying control. Links, nicknames, quotes, and reaction buttons keep their own actions; drags do not open the menu. Right-click members for Whois/private-message actions. Server capability and ownership rules still apply. Read markers represent your own reading position, not recipient read receipts.

## Navigation and shortcuts

The thin left rail selects a server. Its avatar falls back to initials, its connection dot has a textual state tooltip, and its selected marker is not color alone. The adjacent sidebar shows that server's channels and direct messages. Switching servers restores their last usable conversation; selecting the same server does not change sidebar intent. The topbar navigation button cycles **expanded → avatar-only → hidden**; the server rail remains accessible.

Server logs do not occupy channel rows or compact channel avatars. Right-click the corresponding server avatar and choose **Open server log**, including for a saved profile that has not connected yet. The log header identifies its source network; the global conversation picker can still reach retained logs.

Drag the dividers beside the server rail and channel sidebar to resize them. Expanded and avatar-only channel widths are independent; icons scale with their available space. Drag updates follow native frames, and the final widths save on release rather than on every pointer motion. Widths survive restart, while narrow-window fitting leaves your preferred widths and panel modes intact.

The title logo shares the centered square footprint of server avatars and scales with the fitted rail width.

Settings and Add network each have one button at the bottom of the server rail. **Mark all as read** appears immediately above Add only while a non-search conversation has unread messages; it acknowledges all conversations, including retained disconnected networks, without clearing drafts or history. The topbar contains the app logo/name, navigation, **Commands** palette, and close; window-manager shortcuts and titlebar double-click still handle window management. The titlebar and left bars share one shell surface.

There is no tab bar. `C-x j` opens Avy-style hints for the selected server's conversations: the UI dims and letter codes appear beside channels or their compact avatars. Hidden navigation temporarily shows the avatar strip for hints, then returns to hidden after selection or cancellation. Type the displayed code to switch; colliding initials receive additional keys. Hints use literal ASCII letters/digits, case-insensitively. Escape or the configured Cancel chord leaves the draft unchanged. Paging and scrolling retain access to offscreen choices. `C-x b` remains the global conversation picker.

The conversation title shows an avatar, channel/peer name, and a small network chip when channel navigation is compact or hidden. It is not a picker dropdown. A channel's topic icon beside **People** opens a selectable, read-only formatted topic popover, including an empty-topic state. Topic updates do not generate transcript announcements; an explicitly correlated `/topic` response can still return to its originating conversation, and stored historical topic lines are retained. Narrow layouts keep conversation actions accessible through More instead of squeezing the identity between redundant controls.

The channel sidebar retains loaded-name search and All/Unread/DMs filtering, plus **Archived** recovery in expanded and compact navigation. Regular filters pin the active conversation when needed, but archived chats stay out of normal lists and Avy hints. Filtering neither fetches history nor changes the active conversation/read state. Tooltips show bounded local draft/message previews; unread and pending indicators retain their actual meaning.

Channels and DMs sort by actual chat activity, including received messages, own sends, and `/me`, not typing, presence, or history loading. **Pin/Unpin** in conversation menus persists a stable pinned group, newest pin first; unpinning returns the chat to activity order.

The rail's **@** button opens global **Mentions**, a read-only, newest-first view of retained received mentions across networks. Legacy history is classified only when the source network's current nickname is known. Rows identify their source actor, network, and conversation; activate that context or **Open original** to jump to the retained source message without altering its draft. The feed does not mark sources read, and source-aware privacy guards still apply. In ordinary conversations, floating **↑ @count ↓** controls move to older/newer readable mentions without wrapping.

Right-click a channel or DM, or use its conversation overflow menu, to **Mute** or **Archive** it. Mute suppresses notifications and sounds, not unread counts or incoming visual effects; the stricter Fools policy remains independent. Archive does not part, disconnect, delete history, or discard drafts. Open **Archived** to recover chats and use Unarchive/Unmute; merely opening a chat does not unarchive it. A new live incoming message returns an unmuted archived chat to the normal list; muted chats stay archived. History/replay, own sends, and fool messages do not bring archived chats back. Unmuting alone does not restore one; the next eligible incoming message can.

The right member list independently cycles **expanded → avatar-only → hidden** with its existing button. Expanded members show avatars beside nicknames; compact initials retain privilege badges, full-name tooltips, and member actions. DMs/utility views suppress the list without changing its preference. Narrow windows prefer compact strips before hiding members; restoring width restores eligible panels without overwriting either sidebar's saved choice.

Channel names use monospace text with avatar/initials icons and separate draft overlays. On servers supporting [IRCv3 metadata](https://ircv3.net/specs/extensions/metadata), the registered channel [`avatar` key](https://ircv3.net/registry#channel-metadata) can supply an image URL with an optional `{size}` substitution. Loading is asynchronous and controlled by the existing **Avatars** preference. Missing, disabled, invalid, or unavailable images fall back to initials.

Remote channel avatars contact the image host directly and expose the viewer's IP and request timing. Only public HTTPS destinations are allowed, with verified TLS, transfer/dimension limits, and PNG/JPEG/static WebP preflight. Redirects, proxies, credentials, cookies, animated images, and automatic retries are not supported. Images use a private session cache, not persisted channel configuration; after restart they are refreshed from server metadata.

The bottom status bar groups the active nick, network, connection, feedback, and pending key chord with icons, semantic colors, and quiet separators. **Connected** with a plug icon means the active network has finished IRC registration; it is not your away status. Long names elide before connection or failure feedback. Unread counts in conversation and server navigation use solid opaque primary rounded-square plates, with distinct error plates for mentions and palette-repaired readable foregrounds, even when interface transparency is enabled.

Only while channel navigation is actually **Hidden**, the status bar also summarizes unread non-archived channels by alias/`#channel` and count. Nickname mentions add a bold **@** and error color; ordinary counts use primary color. Expanded, Compact, and temporary Avy Compact navigation suppress this summary. It uses the same unread/Fools read policy as navigation; muting does not remove counts. There are no additional buttons or settings.

`C` means Ctrl, `M` means Alt. For `C-x b`, press Ctrl+X, release, then B.

| Shortcut | Action |
| --- | --- |
| `C-x b` | Open/select a conversation |
| `M-x` | Open the **Commands** palette |
| `C-x j` | Show selected-server conversation hints |
| `C-s` | Search current conversation |
| `C-,` | Open settings |
| `C-g` | Cancel and clear transient status feedback (retaining nick/network/connection indicators) |
| `M-p` / `M-n` | Browse composer history |
| `C-p` / `C-n` | Select the previous/next message |
| `PageUp` / `PageDown` | Scroll half the message viewport; preserve modal/editor paging |
| `C-r` | Reply to the selected message |
| `C-e` | Move to logical hard-newline end in a text box; open fuzzy reactions only with message focus |

Bindings are editable in **Settings → Keyboard**, in a grouped responsive table with aligned Action, Current, Default, Record, and Reset columns that stack in narrow layouts; recording reports conflicts. `C-t` transposes graphemes. Backward kill-line and other native edit commands are configurable, with conflicting defaults left unbound. Existing `C-e` reactions, `C-n`/`C-p` message navigation, `C-s` search, `C-x` prefixes, and `C-u` semantics are retained. The hint shortcut remains unbound if its default would shadow an existing configured chord. `toggle-navigation` and `toggle-people` cycle the two sidebars through their three states. Removed tab/split/dock/float commands are not layout options.

After `C-p`/`C-n` gives a message navigation focus, Enter opens its URL chooser, even for one link; it does not open a link automatically. Dropdown menus support `C-n`/`C-p` and Enter, and the Enter held while opening a menu cannot activate an item. `C-g` clears transient status feedback while preserving existing cancellation, IME, and draft behavior and structural nick/network/connection indicators.

Searching again from a search view retains the original conversation separately for each network, including soju searches. Source identity is validated across renames and closure rather than treating the search buffer as the original target.

The native composer keeps per-conversation drafts, selection, undo, history, clipboard, and IME state in one rounded input surface. Its empty and typed single-line height is stable; wrapped text grows within its line budget, then scrolls internally. The `+` button raises a formatting toolbar with bold, italic, underline, strikethrough, monospace, reverse/reset, and sampled foreground/background colors. IRC formatting renders in the draft while the actual IRC control bytes are preserved for sending. Formatting controls and clipboard shortcuts must not discard active IME composition.

Ordinary typing from the transcript or non-editing chat controls refocuses the active visible composer and keeps the first character, including Shift and Unicode input. Configured hotkeys and pending prefixes take priority; Space still activates a focused control. Search, settings, other text fields, modal overlays, active composition, and read-only views retain their own input ownership. A first dead-key preview is not redirected; its eventual composed character can refocus the composer.

The composer matches the actual active conversation's message font and text size, not the interface scale. Known nicknames receive display-only nickname colors, with explicit mIRC formatting taking precedence; raw draft bytes, copy, and undo are unchanged. In native text boxes, `C-e` moves to the logical hard-newline end, not the end of a soft-wrapped visual line; reactions use `C-e` only with message focus.

Right-click selected composer text for native clipboard actions and a formatting submenu. Actions retain the source selection and use the same undoable IRC formatting operations as the toolbar; active IME composition is not discarded.

Offline Hunspell spelling is enabled by default, with bundled English (`en_US`). **Settings → Appearance → Composer spelling** offers **Check spelling** and **Spelling language** using available dictionaries. Auto follows `LANG`, using English for C/POSIX or no locale; a missing dictionary is visibly unavailable, not silently replaced by another language. Misspellings have native error underlines; right-click for suggestions and explicitly choose a correction as one undoable edit. Checking never auto-replaces text or sends it over the network. URLs, nicknames, channels, code, slash-command syntax, ghost text and active IME preedit are excluded. `[composer]` stores `spellcheck` (default `true`) and `spellcheck_language` (default `"auto"`).

Type `:query` for fuzzy emoji suggestions from the bundled catalog's names, keywords, and common aliases. Up/Down selects; Enter or Tab inserts the Unicode emoji as one undoable edit. Escape dismisses without changing the query. URLs, times, selections, and active preedit do not open suggestions; no image service or network request is involved.

Nickname Tab completion uses the same popup interaction as command suggestions, preserving the untouched raw suffix, undo history, and IME state.

Drag message body text to highlight it; copy preserves the composer's draft and selection. `C-p`/`C-n` navigate messages without editing the draft, using a rounded message-shaped selection around the bubble or IRC block. `C-r` and message-menu Reply show a parent preview on every server, including when the parent has only a stable local identity. Composer previews and in-message quoted parents use rounded, fully outlined cards with a nick-colored accent rail, bold author, and formatted excerpt. The transparent composer card has two total lines—author/header and excerpt—and a stable transcript-overlay allocation above the typing row, independent of paste previews. Only the composer card has Cancel. Sending uses an IRCv3 reply tag only when currently supported and the parent has a usable wire ID; otherwise it sends a `nick: ` prefix without changing the raw draft or inventing unsupported tags. Cancel removes reply context while leaving draft text intact. With message focus, `C-e` opens a searchable emoji picker for a supported reaction; closing it restores the original focus. Targets follow the selected message, not whichever message arrived most recently.

Reply context/cancel and Send remain accessible. A multiline paste shows its destination and choices directly before separate sending; insertion remains ordinary editable draft text. These decisions do not hide behind the formatting button.

Typing uses bare nickname-colored text without chips or backgrounds in a stable row above the composer. The typing row sits below the reply card as a transcript overlay, without shifting the input or transcript viewport; the receive-typing preference still controls visibility.

### Upload a file, text, or clipboard snapshot

The dim paperclip appears only in an empty, non-composing input. Choose **File**, **Text / paste**, or **Clipboard** as the source. A usable default host goes directly to editing/review; **Choose host** remains optional. File uses the system chooser; Text / paste opens a wrapped, scrollable multiline editor where Enter inserts a newline, followed by **Review text → Upload**. Clipboard takes a bounded snapshot, preferring supported image data to text. Files and images receive an explicit local pre-upload preview: a bounded PNG/JPEG/WebP image or GIF first frame where supported, otherwise honest metadata. Review the source, size, destination and public-link disclosure before **Upload**. Pasting an image also opens review, even with a nonempty draft; nothing uploads or sends automatically. Host changes retain staged bytes, and draft/IME/source guards remain in force. Progress and cancellation remain accessible while typing or switching conversations.

Configure the single default host and provider-specific options in **Settings → Chat → File uploads**. Available destinations are CrafterBin, x0.at, Uguu, Litterbox, Catbox, paste.c-net.org, a Custom compatible HTTPS endpoint, and an available soju FILEHOST. A per-upload choice does not change that default unless **Make default** is selected. The default size limit is 25 MiB; Custom permits up to 512 MiB and x0.at up to 1 GiB. Secret links are unlisted, not private.

Uploads stream with verified HTTPS. Custom endpoints support optional credentials; changing their authority clears the saved credentials. Soju uploads require live authenticated child/account sessions, a configured account password, and an advertised HTTPS host belonging to the physical bouncer's hostname or subdomains, or the account's exact explicitly trusted file-host hostname. **The upload host receives the parent account's actual username and password through HTTP Basic authentication. Trusting an additional hostname grants it those credentials, not just permission to receive files.** An EXTERNAL-only account without a configured password cannot use FILEHOST. An unavailable soju host never silently falls back to a public service.

A successful link is inserted as one undoable edit into the original unchanged draft, never automatically sent. If that document changed, is composing, merged, or disappeared, the completed link remains available for explicit insertion or copying instead of overwriting another conversation. Cancellation and retry do not automatically replay an upload; a file may already exist remotely if cancellation races completion.

The system chooser requires a working **xdg-desktop-portal FileChooser** backend. It is unparented, so its placement is compositor-managed. Upload credentials are stored as plaintext in the private, mode-0600 preferences file; they are not encrypted.

Text and clipboard bytes use private, size-bounded temporary files owned by the upload flow and removed when no longer needed; user-selected files are never removed. Completed links remain bound to the originating draft and recovery controls, not whichever conversation is currently visible.

### Link and media previews

**Settings → Appearance → Link and media previews** controls links, images, and audio/video separately: **Off**, **Click to load**, or **Automatic**, all defaulting to Click to load. The first preview uses a thumbnail or honest audio/video/generic tile, with a default maximum height of 120 pixels and a compact caption below. HTML/link previews retain a visible URL, including when they have an `og:image`; image/audio/video captions use a real title when known or the URL otherwise. There is no separate **View image** or **Open player** button. Image/media clicks open the viewer/player; HTML/link clicks open Info. Right-click offers **Info** with a selectable URL and known metadata, **Copy**, and **Open Link**. Before loading, the tile is an explicit load affordance, not a fabricated thumbnail. A URL-only body is visually replaced only by an actually loaded, visible preview; surrounding text and raw copying remain intact. Supported audio/video file assets open a paused player with play/pause, seek, time, volume, and close. Playback is explicit, never autoplay. Closing or invalidating the source stops playback; live readability guards also prevent concealed content from remaining exposed. This is not a general website/video-service embed: unsupported formats and playlists are not played.

Image previews support bounded PNG/JPEG, static WebP, and a static first frame of GIF. APNG and animated WebP are explicitly unsupported. HTML metadata appears through Info; audio/video use the paused player. Preview failures show the HTTP status and reason when available.

Loading contacts the remote host and exposes your IP and request timing. Preview requests are anonymous bounded GETs, separate from upload credentials, with verified HTTPS certificates, no cookies, ambient credentials or proxies, and revalidated redirects/DNS/socket peers. Public destinations are required by default; **Allow local-network preview requests** explicitly permits private/loopback services, including metadata endpoints, and can expose local services or private data to posted links. Automatic modes also use this permission; leave it off unless you trust the links and network. It is not a TLS bypass: platform certificate trust remains authoritative. Type-specific privacy applies to linked images too: loading a page does not silently authorize its image. Downloads, raster dimensions, cache files and playback duration are capped; media defaults are 64 MiB and 600 seconds. No preview requests are made for concealed fool content.

## Make it yours

### Message style

Channels default to content-fit bubbles: incoming left, your messages right, with compact nick-colored sender chips above the body and your own sender heading right-aligned. Only uninterrupted messages from the same sender group together; another sender, event, or day boundary breaks the group. Internal gaps separate messages, with larger gaps between groups. Nicknames are smaller than message text; timestamps are smaller still, dimmed, and separated in the bottom-right footer. Short names receive their measured width instead of unnecessary truncation; genuinely long names remain bounded. Bubbles fit the measured header, body, and timestamp within the wrap/maximum width rather than reserving a large minimum. A spinner means queued; a single check means the complete message reached the socket, not that another person read it. Server echoes can confirm the send. After restart, unconfirmed local sends recover as editable drafts without automatic resending; whether the server received them remains uncertain. Set an individual channel to **traditional IRC rows** for denser reading; its choice persists across restarts and does not affect other channels. Switching style changes rendering, not history or drafts.

Day separators are centered chips. Structured IRC join, part, quit, and nick-change events use quiet monospace activity chips: **Smart** groups related notices and expands them on click, **All** keeps them individual, and **Hidden** suppresses them. Smart uses event time: own events, someone who contributed chat in the preceding 300 seconds, or someone with at least five contributions in seven days is eligible; forced kick/ban notices remain visible. Current topics belong in the header popover, not generated announcements for 332/331/TOPIC updates; historical topic lines are not deleted. It does not infer presence from bot names or message-text regular expressions. User-authored `/me` renders as an italic emote line and remains conversation, not a presence notice.

The “new since your last visit” divider appears before unread incoming chat, never as an empty separator at the tail. Focus changes, own sends, typing, presence notices, and loading older history do not create it; genuine persisted unread survives reopening the app. The divider stays hidden while the window is focused and clears when those messages are read. This does not acknowledge unseen history: scrolling away from the tail still preserves unread counts and your durable reading position.

Use the conversation menu, or enter `/messagestyle irc` and `/messagestyle bubbles`. With no argument, `/messagestyle` toggles the current conversation's style. Server logs and search views remain utility rows.

Traditional IRC rows reuse the existing nickname-avatar style. Wide chat panes keep timestamp, avatar, nickname, and body inline. Narrow chat panes put timestamp/avatar/nickname in a header on every message, with the body below; the available chat-pane width, not window width, decides. Repeated unchanged formatted timestamps are suppressed while retaining their space; day changes reset suppression. Your own rows use an accent nickname chip with a small outgoing arrow instead of the old left-side line; the arrow identifies ownership, not delivery or a read receipt.

Both bubbles and traditional IRC rows wrap at 80 user-visible grapheme characters by default, at word boundaries where possible; the viewport may wrap earlier and long words split only at grapheme boundaries. Set the wrap limit to **0** for no character cap, still bounded by the viewport; an existing explicit unlimited setting is retained. This is presentation only: raw message text and copied whitespace are unchanged, with no inserted wrap newlines. Known nicknames inside messages are bold nick-colored text without a chip or background; sender-name chips remain, and links keep their own actions.

**Fools** is a local nickname-based filter using the source server's IRC casemapping, not a server-side block or authenticated-identity rule. Default **Minimized** groups can be clicked to reveal; **Blurred** bodies also require reveal, **Dimmed** stays readable, and **Hidden** omits rows. All retain history. Concealed bodies are masked in search, reply context and sidebar previews, and cannot leak through selection/copy or media previews before reveal. Fool notifications, sounds, typing and incoming effects are muted; unread increments are also muted by default, with an optional **Unread** setting.

The conversation menu's **Show fools / Hide fools**, or `C-x f`, persists a per-channel/DM override without changing the global policy. Global Mentions follows each source conversation's current Fools policy rather than a separate feed override.

Ordinary activity summaries never acquire fool blur merely because they are collapsed, deleted, or unreadable. Genuine fool concealment and reveal rules remain independent; minimized fool summaries reserve enough space to avoid overlapping neighboring IRC rows.

The interface uses one modern messenger surface. Noctalia supplies semantic surface, border, selection, and accent colors. Controls use light separators and consistent rounded corners without a heavy frame around the chat. Per-channel IRC rows remain independent of fonts and theme colors; obsolete Modern/Glass/Hacker interface presets are no longer settings.

### Appearance and transparency

Choose UI/message font families, avatars, and nickname colors in Settings. **Interface scale** and **Message text scale** are independent, each ranging from 0.7 to 2.0 with default 1.0. Interface scale sizes chrome; message text scale sizes message bodies, metadata, and the matching composer text without changing either font family. Bubble bodies use `14 × message scale`, while dense IRC/utility bodies use `15 × message scale`; the composer follows the active buffer's actual size. Independent UI and message font pickers fuzzy-filter installed Fontconfig families while typing without applying the query. An explicit installed-family choice or Enter commits a family, including an arbitrary typed family; closing discards an unapplied query. Empty font overrides use sans-serif; explicit font choices are retained. Native shaping/rasterization keeps font fallback and color emoji. Native output density remains separate from both user scales.

The native minimum width remains 720; minimum height is font/scale-aware with a floor of 480, normally 960 at 2× interface scale. This is not a promise to fit 2× controls into a 720×480 window; the compositor remains authoritative over the actual window size.

**Transparency is off by default.** Enable it explicitly to use the configured surface-opacity intensity across background surfaces, including panels, bubbles, controls, menus, and dialogs. Surface opacity does not fade text or icons. Your Wayland compositor must support desktop alpha; low opacity can reduce contrast against wallpaper.

### Motion, sound, and settings

Choose Off, Subtle, or Full effects and tune individual sound cues. Effects Off suppresses event motion, caret trails, and event sounds. Reduced motion suppresses animation without muting enabled sound or overwriting static preferences. New settings start at Subtle; existing explicit effects choices are preserved.

Sending and incoming effects have independent motion and reveal controls in **Settings → Effects**. Sending offers **None, Flight, Rise, Pop, Slide, Drift** and **None, Fade, Pixels**, default Flight/Pixels. Under **Incoming messages**, **Incoming movement** excludes Flight, **Incoming reveal** offers None/Fade/Pixels, and **Incoming duration (100–2000 ms)** adjusts their duration; defaults are None/Fade over 550ms. Slide enters from the side; Drift settles gently. `[appearance]` stores `send_motion`, `send_reveal`, `incoming_motion`, `incoming_reveal`, and `incoming_duration_ms`. Loaded history, scrolling, resizing, theme refreshes, and server echoes do not replay entrances. Effects Off and Reduced Motion show the settled conversation immediately.

Settings has seven categories: **Appearance, Keyboard, Chat, Effects, Sounds, Networks, Identities**. Global fuzzy search finds settings, sections, categories, and command names across all seven pages and jumps to the matching control; it does not search current values or secrets. Bold section headings, explanatory text, grouped containers, and separators organize each page. Keyboard-navigable categories become a native selector in compact windows; details scroll separately from Save/Close. Mouse-wheel input scrolls the page rather than changing slider values; keyboard and drag adjustment remain available.

Valid edits apply live and autosave. Invalid/incomplete profiles are not written and retain visible validation errors. Save-and-connect saves successfully before starting the connection. Close is not rollback.

### Noctalia colors

The [included template](noctalia-template/) supplies semantic colors such as `surface_container_low`, `surface_container_high`, `on_surface`, `outline_variant`, paired accent containers, and authored `hover`/`on_hover` roles. Hover colors are not assumed to equal tertiary colors. The native library is bundled; installing the Noctalia shell is optional. A built-in palette works without it.

Keep **Theme source** set to `auto` to use `$XDG_CONFIG_HOME/blastirc/noctalia.toml` when present (normally `~/.config/blastirc/noctalia.toml`). Generated palette changes reload locally while the app runs.

Noctalia's **Pure black dark** setting is applied before live template generation: the backdrop becomes black and elevated surfaces use Noctalia's very-dark surface ramp while accents remain authored. If generating manually with `--theme-json`, also pass `--pure-black` when that setting is enabled. Do not apply a second blackening transform to the generated app palette.

To register the template locally:

1. Copy `noctalia-template/` to `<Noctalia config>/templates/blastirc/`.
2. Copy its `user.toml` to `<Noctalia config>/blastirc.toml` to register `[theme.templates.user.blastirc]`.
3. Enable **blastirc** in Noctalia **Settings → Templates**.

Use Noctalia's resolved config directory, including overrides. Noctalia 5.4.1 local user templates ignore `undo_hook`; when removing/disabling the template, run `sh <Noctalia config>/templates/blastirc/undo.sh` manually to remove only the generated palette and return to built-in colors. The standalone/catalog target retains undo metadata for consumers that support it.

## Your data

Configuration and history follow XDG directory settings.

| Default path | Contents |
| --- | --- |
| `~/.config/blastirc/config.toml` | Network, soju parent/child, and Identity profiles |
| `~/.config/blastirc/settings.toml` | Preferences, file-host settings and optional credentials, active selection, sidebar state, per-channel message style |
| `~/.config/blastirc/noctalia.toml` | Optional generated palette |
| `~/.local/share/blastirc/state.sqlite3` | Local history, drafts, and chat state |

Per-buffer editing and view state are retained without an ordered tab layout. Obsolete tab order, split trees, floating geometry, and legacy `layout.ini` import are removed. Drafts and history remain in SQLite.

SQLite schema 9 retains per-chat mute/archive flags and the structured IRC activity/nick-change targets introduced in schema 8; older supported databases migrate with mute/archive off by default. Participation-cache revisions are runtime-only. This is separate from TOML configuration schema 1.

Default history retention is 30 days and 100,000 messages globally, not per channel. Clearing local history does not delete server/bouncer history. Saved passwords remain plain text in the private configuration file.

Submitted text is backed up before network-worker admission. Rejected/interrupted sends recover as editable drafts after failure/restart, never automatic replay. Pending history is not proof of delivery. Normal input admits up to 64 KiB; larger restored/recovered drafts are preserved without truncation and can be reduced by editing.

Caret settings under `[cursor]` include `shape` (`line`, solid `box`, `underline`) and `effect` (`off`, `trail`, `comet`, `ribbon`, `pulse`). The animated effects share `trail_duration_ms` (40–240, default 120) and `trail_strength` (0.05–0.35, default 0.20). The box uses native shaped cell width and paints a readable inverse glyph; IME keeps its thin insertion rectangle. Default Line/Off. Motion suppression does not overwrite these choices.

## Development

C++23 with the native [Noctalia library](https://github.com/trevarj/noctalia/tree/ad60f4c2a8841ce25c78f9d6ac8c111523fa34b6/lib), pinned to the maintained fork commit `ad60f4c2a8841ce25c78f9d6ac8c111523fa34b6`. The published fork contains 14 focused signed commits over the unchanged upstream base `5e4aae910b764c7aeee411f741a266263f104375`, including generic scroll-into-view using the public border-aware viewport. The frontend uses retained controls, native Wayland clipboard/IME, and EGL/GLES. OpenSSL, SQLite, libcurl avatar/upload/preview transport, libxml2 HTML metadata parsing, libmpv file-asset playback, GIO's system file-chooser portal, and the IRC core remain independent of the UI library.

For a fresh checkout:

```sh
nix develop path:.
meson setup build-native --wrap-mode=nodownload \
  -Dfont_dir="$BLASTIRC_FONT_DIR" -Demoji_dir="$BLASTIRC_EMOJI_DIR" \
  -Ddictionary_dir="$BLASTIRC_SPELL_DICT_DIR"
meson compile -C build-native
meson test -C build-native --num-processes 1 --print-errorlogs
```

Development tools live in the pinned shell. Existing builds normally regenerate automatically; when migrating an old SDK build directory, rerun the setup command with `--wipe` to discard cached subproject options.

The maintained SDK fork keeps Noctalia's upstream history, identity, MIT license, and credits. In the fork checkout, use `git fetch upstream`, review changes, and selectively import suitable commits with `git cherry-pick -S`; keep new fork commits signed and push them to the fork's `blastirc` branch. To update BlastIRC, pin the same published full commit in `subprojects/noctalia-ui.wrap` and `flake.nix`, updating both the archive SHA-256 (`source_hash`) and unpacked `fetchzip` SRI hash. There is no local patch application or root build-file replacement; BlastIRC selects `library_only=true` and owns its asset/license installation paths.

Package checks use the pinned standard Fontconfig `conf.d` aliases, exclude impure font directories, and isolate XDG configuration, data, and cache in private directories. Application runtime Fontconfig behavior is unchanged.
The media player check uses libmpv's null audio output for real decoding and playback without an audio device or server; application playback still uses system audio.

Real native GUI/IRC smoke workflow:

```sh
nix develop path:. -c python3 tests/gui_smoke.py build-native/blastirc
```

The fixture owns private IRC servers, isolated XDG state, and a headless Sway compositor. It must not borrow the desktop display or modify the user's profiles. Read `--help` for current scopes and capture options. Automated checks are distinct from visual inspection and compositor transparency verification.

Refresh-specific targets include `application`, `network`, `storage`, `effects_envelopes`, `upload`, `clipboard_check`, `preview`, `media_player`, `native_settings_check`, `native_composer_check`, `native_transcript_check`, and `native_shell_check`. `tests/interface_refresh_smoke.py` provides an isolated native refresh workflow; read its current help. Listing a target does not claim it has passed.

Same-window compositor-density workflow:

```sh
nix develop path:. -c python3 tests/dpi_smoke.py \
  --output-dir /tmp/blastirc-dpi-results --zoom 1 \
  -- ./build-native/blastirc
```

Choose a fresh output directory. The app also accepts `--size WIDTHxHEIGHT`, `--frames N`, and `--screenshot FILE.bmp` for bounded native launches. BMP capture drops alpha; use a compositor capture to inspect desktop transparency.

The current architecture and acceptance contract are in [UI-DESIGN.md](UI-DESIGN.md). Previous split-workspace verification does not establish correctness of the flat layout. Report exact exercised checks and remaining limits rather than carrying historical pass claims forward.

## Contributing

Include desktop environment, reproduction steps, server/bouncer version, and whether the connection is direct or through a bouncer. Keep changes focused and preserve protocol/security boundaries.
