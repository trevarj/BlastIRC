#!/usr/bin/env python3
"""Focused real native refresh consumers on disposable Sway/IRC/TLS fixtures.

Run inside nix develop: python tests/interface_refresh_smoke.py BINARY --scope NAME.
Each scope owns a fresh private compositor/profile; no desktop or production profile
is reused. BLASTIRC_SMOKE_SCREENSHOT selects the artifact basename.
The startup-history-sync scope holds genuine private Ergo history on the wire;
it records cached-chat, pending-header, and released-history native evidence.
The jump-bottom scope proves the zero-arrival native FAB click and resumed following.
"""
import argparse
import json
import sys
from pathlib import Path

import gui_smoke as gui
import upload_soju_smoke as upload


def run_smart_presence(ctx):
    """Wire-retained idle lifecycle disappears only from the actual Smart viewport."""
    import os
    import sqlite3

    p, w = ctx['proc'], ctx['window']
    prefs, db = ctx['preferences'], ctx['database_path']
    channel, identity = ctx['channel'], ctx['channel_id']
    observer = ctx['observer']
    _policy_focus(ctx, identity)
    original = tomllib.loads(Path(prefs).read_text())['presence']['channels']
    nick = 'sp' + os.urandom(4).hex()
    colors = ((45, 155, 217), (217, 69, 119), (42, 185, 113), (179, 79, 213))
    bodies = [
        '\x042D9BD9,2D9BD9SPSTART\x0f',
        '\x04D94577,D94577SPWORDS\x0f ordinary chat: joined the room; away for lunch',
        '\x042AB971,2AB971SPSPOKE\x0f real participant contribution',
        '\x04B34FD5,B34FD5SPEND\x0f',
    ]
    wire, phases = [], []

    def rows(sql, params=()):
        with sqlite3.connect(f'file:{db}?mode=ro', uri=True) as connection:
            return connection.execute(sql, params).fetchall()

    frontier = rows('SELECT COALESCE(MAX(seq),0) FROM messages')[0][0]
    typed = ('SELECT activity FROM messages WHERE buffer=? AND seq>? AND nick=? '
             'AND presence=1 ORDER BY seq')

    def receive(command, params):
        gui.receive_until(observer, command, params, nick, redact=ctx['lab']._redact)
        wire.append(observer.frames[-1])

    def mode(value):
        root = gui.open_title_menu(w, gui.title_actions(gui.read_layout(prefs), ctx['args'].zoom))
        child = gui.open_native_submenu(w, root, 3, from_end=True)
        gui.capture_native_popup(w, child, Path(ctx['destination']), 'smart-presence-menu-' + value)
        gui.popup_keys(w, root, 'Home', *(['Down'] * ('global', 'all', 'smart', 'hidden').index(value)), 'Return')
        expected = dict(original, **{identity: value})
        gui.wait_preference(p, prefs, 'presence', 'channels', expected)

    def capture(value):
        mode(value)
        state = gui.save_state(p, prefs, db)
        assert state[1] == (identity,), ('Wrong native target during presence proof', state)
        gui.wait_layout(p, prefs, predicate=lambda layout: 'settings' not in layout)
        read = gui.wait_read_marker(p, db, identity)
        assert read[3] == bodies[3], read
        rectangle = gui.read_layout(prefs)['conversation']['rect']
        previous = None

        def settled():
            nonlocal previous
            assert p.poll() is None, 'Native Smart consumer exited'
            image = gui.native_rgb_capture(w)
            components = [gui.native_marker_components(image, rectangle, color) for color in colors]
            if any(len(items) != 1 or not items[0]['full'] for items in components):
                previous = None
                return False
            boxes = [items[0]['bbox'] for items in components]
            stable = previous == boxes
            previous = boxes
            return boxes if stable else False

        boxes = gui.SESSION.until('all four complete settled Smart body anchors in viewport', settled)
        assert all(a[3] < b[1] for a, b in zip(boxes, boxes[1:])), boxes
        path = gui.capture_window(w, Path(ctx['destination']), 'smart-presence-' + value)
        result = {'mode': value, 'boxes': boxes, 'idle_gap': boxes[1][1] - boxes[0][3],
                  'participant_gap': boxes[3][1] - boxes[2][3], 'capture': str(path),
                  'selected': list(state[1]), 'tail_read_marker': read}
        phases.append(result)
        return result

    added = sorted({'away-notify'} - observer.capabilities)
    if added:
        observer.send('CAP REQ :' + ' '.join(added))
        observer.wait(lambda command, params, prefix: command == 'CAP' and
                      ('ACK' in params or 'NAK' in params))
    assert 'away-notify' in observer.capabilities, 'Private IRC server lacks real AWAY witness'
    mode('smart')
    _policy_send(ctx, bodies[0], target=channel)
    with gui.fixtures.BootstrapClient(
            dict(ctx['lab'].endpoints['direct'], nick=nick, username=nick),
            ctx['lab'].ca_file, authenticate=False) as guest:
        guest.send('JOIN ' + channel)
        receive('JOIN', [channel])
        gui.wait_rows(p, db, typed, (identity, frontier, nick), [(1,)])
        guest.send('AWAY :private smart away')
        guest.wait(lambda command, params, prefix: command == '306')
        receive('AWAY', ['private smart away'])
        guest.send('PART ' + channel + ' :private idle departure')
        receive('PART', [channel, 'private idle departure'])
        guest.send('JOIN ' + channel)
        receive('JOIN', [channel])
        gui.wait_rows(p, db, typed, (identity, frontier, nick), [(1,), (2,), (1,)])
        _policy_send(ctx, bodies[1], target=channel)
        guest.send(f'PRIVMSG {channel} :{bodies[2]}')
        receive('PRIVMSG', [channel, bodies[2]])
        ctx['expected_messages'].extend(ctx['channel_messages'](bodies[2], nick))
        gui.wait_messages(p, db, ctx['expected_messages'])
        guest.send('AWAY')
        guest.wait(lambda command, params, prefix: command == '305')
        receive('AWAY', [])
        guest.send('PART ' + channel + ' :private participant departure')
        receive('PART', [channel, 'private participant departure'])
        gui.wait_rows(p, db, typed, (identity, frontier, nick), [(1,), (2,), (1,), (2,)])
    _policy_send(ctx, bodies[3], target=channel)
    retained = rows('SELECT seq,nick,text,time,presence,activity,deleted FROM messages '
                    'WHERE buffer=? AND seq>? ORDER BY seq', (identity, frontier))
    lifecycle = [row for row in retained if row[1] == nick and row[4]]
    contribution = [row for row in retained if row[1] == nick and row[2] == bodies[2]]
    assert len(contribution) == 1 and contribution[0][4:6] == (0, 0), contribution
    assert [row[5] for row in lifecycle] == [1, 2, 1, 2], lifecycle
    assert len(retained) == 8, ('Unexpected transcript row, including possible AWAY noise', retained)
    assert [(row[1], row[2], row[5]) for row in retained if not row[4]] == [
        (ctx['query_target'], bodies[0], 0), (ctx['query_target'], bodies[1], 0),
        (nick, bodies[2], 0), (ctx['query_target'], bodies[3], 0)], retained
    assert all(row[6] == 0 for row in retained), retained
    assert all(row[3] < contribution[0][3] for row in lifecycle[:3]), (
        'Idle event timestamps must precede actual participation, not tie it', lifecycle, contribution)
    assert lifecycle[-1][3] >= contribution[0][3], (lifecycle, contribution)
    smart, hidden, all_rows = capture('smart'), capture('hidden'), capture('all')
    assert abs(smart['idle_gap'] - hidden['idle_gap']) <= 1, phases
    assert all_rows['idle_gap'] > smart['idle_gap'] + 6, phases
    assert smart['participant_gap'] > hidden['participant_gap'] + 6, phases
    assert abs(smart['participant_gap'] - all_rows['participant_gap']) <= 1, phases
    assert rows('SELECT seq,nick,text,time,presence,activity,deleted FROM messages '
                'WHERE buffer=? AND seq>? ORDER BY seq', (identity, frontier)) == retained
    mode('smart')
    if added:
        observer.send('CAP REQ :' + ' '.join('-' + cap for cap in added))
        observer.wait(lambda command, params, prefix: command == 'CAP' and
                      ('ACK' in params or 'NAK' in params))
    return _refresh_return(ctx, 'smart-presence', {
        'synthetic_nick': nick, 'wire': wire, 'retained': retained, 'phases': phases,
        'ordinary_chat_body': bodies[1], 'all_anchors_complete_in_same_target': True,
        'idle_smart_matches_hidden': True, 'participant_smart_matches_all': True,
        'away': 'Real AWAY/306 and back/305 with away-notify delivery; no typed Away exists in the model, so no retained Away row is claimed',
    })


def main():
    parser = argparse.ArgumentParser(description=__doc__, add_help=False)
    parser.add_argument("--scope", required=True,
                        choices=("badges", "typing", "typing-bubble", "fools", "transcript", "incoming", "uploads", "media",
                                 "notifications", "commands", "replies", "chat-controls", "irc-polish",
                                 "spelling", "unread-divider", "smart-presence", "activity-blur",
                                 "settings-tree", "settings-refinements", "transcript-refinements", "topic-refinements",
                                 "nick-popup", "whois-profile", "messaging-refinements",
                                 "cursor-affordances", "composer-affordances", "overlay-refinements",
                                 "activity-pins", "input-routing", "logo-alignment",
                                 "minimum-height", "message-links", "typing-focus", "mentions-unread",
                                 "startup-history-sync", "jump-bottom", "buffer-boundaries",
                                 "conversation-history", "soju-search-context"))
    options, remaining = parser.parse_known_args()
    if any(option in remaining for option in ("--probe", "--probes", "--layout-only", "--ime-only", "--raw-only")):
        parser.error("--scope selects the focused fixture; do not combine other GUI scopes")

    def run(**context):
        destination = Path(context["destination"])
        try:
            report = globals()["run_" + options.scope.replace('-', '_')](context)
            gui.wait_messages(context["proc"], context["database_path"], context["expected_messages"])
            artifact = destination.with_name(destination.stem + "-interface-" + options.scope + ".json")
            artifact.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
            print("Native interface consumer evidence:", artifact, flush=True)
            return (context["proc"], context["window"], context["screenshot"], context["expected_messages"])
        except BaseException:
            # Restarts update context before the outer runner receives the new handles.
            # Keep each diagnostic independent; none may replace the original failure.
            label = options.scope + "-current-failed"
            try:
                gui.capture_window(context["window"], destination, label)
            except BaseException:
                pass
            try:
                trace = context["proc"].native_debug_path.read_text(errors="replace")
                destination.with_name(destination.stem + "-" + label + ".wayland.log").write_text(
                    context["lab"]._redact(trace))
            except BaseException:
                pass
            try:
                state = gui.native_input_state(context["window"])
                if state is not None and state["text"] is not None:
                    state["text"] = context["lab"]._redact(state["text"])
                metadata = {"pid": context["proc"].pid, "window": context["window"],
                            "native_input": state}
                destination.with_name(destination.stem + "-" + label + ".json").write_text(
                    json.dumps(metadata, ensure_ascii=False, indent=2) + "\n")
            except BaseException:
                pass
            raise

    # Reuse the existing owned fixture and teardown, not a second GUI harness.
    gui.run_advanced_scope = run
    sys.argv = [sys.argv[0], *remaining, "--probe", "shell"]
    gui.run(allow_focused_geometry=options.scope == 'typing-bubble')


"""Sections for interface_refresh_smoke.py; only owned gui_smoke fixture state is used.

Runtime dependencies: existing gui_smoke/upload_soju_smoke dependencies, Pillow,
ffmpeg (lavfi, PCM WAV and MPEG-4 encoder), wl-copy, and tesseract.
"""
from email.parser import BytesParser
from email.policy import default
import base64
import http.server
from pathlib import Path
import re
import ssl
import subprocess
import threading
import time
import tomllib

from PIL import Image, ImageChops
import gui_smoke as gui
import upload_soju_smoke as upload


def _refresh_restart(ctx, section, values, *, focus=None):
    """Set explicit private-fixture policy, then restart the real native consumer."""
    gui.save_state(ctx['proc'], ctx['preferences'], ctx['database_path'])
    target = Path(ctx['destination']).with_name('refresh-before-' + section + '.bmp')
    out, err = gui.finish_gui(ctx['proc'], ctx['window'], ctx['screenshot'], target, ctx['args'].size)
    ctx['logs'].append(('refresh-before-' + section, out, err))
    departed = {ctx['own_nick'], ctx['other_nick']}
    while departed:
        _, _, prefix = ctx['observer'].wait(lambda command, params, prefix:
            command == 'QUIT' and prefix.split('!', 1)[0] in departed, timeout=15)
        departed.remove(prefix.split('!', 1)[0])
    preferences = Path(ctx['preferences'])
    text = preferences.read_text()
    # Replace only this generated private profile's table, preserving workspace state.
    block = '[' + section + ']\n' + ''.join(f'{key} = {value}\n' for key, value in values.items())
    pattern = r'(?ms)^\[' + re.escape(section) + r'\]\s*\n.*?(?=^\[|\Z)'
    text = re.sub(pattern, '', text)
    preferences.write_text(text.rstrip() + '\n\n' + block)
    preferences.chmod(0o600)
    screenshot = Path(ctx['screenshot']).with_name('refresh-' + section + '-restart.bmp')
    proc, window = gui.launch_gui(ctx['binary'], screenshot, ctx['env'], ctx['args'].size, ctx['logs'])
    ctx.update(proc=proc, window=window, screenshot=screenshot)
    gui.wait_joins(ctx['observer'], ctx['channel'], (ctx['own_nick'], ctx['other_nick']))
    if focus is None:
        gui.switch_buffer(ctx['channel'])
    else:
        _policy_focus(ctx, focus)
    upload.focus_composer(window)
    gui._composer_native(window, '', 0, 0)


def _refresh_return(ctx, scope, evidence):
    return {'scope': scope, 'native_consumer': True, 'private_fixture_only': True,
            'evidence': evidence}


def _refresh_clipboard(ctx, data, mime):
    child = subprocess.Popen(['wl-copy', '--foreground', '--type', mime], env=gui.SESSION.env,
        stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        start_new_session=True)
    ctx['cleanup'].callback(gui.stop_group, child)
    child.stdin.write(data)
    child.stdin.close()
    def offered():
        # A previous offer may advertise the identical MIME. Verify the actual
        # private bytes before posting paste, as gui.seed_clipboard does for text.
        result = subprocess.run(['wl-paste', '--no-newline', '--type', mime], env=gui.SESSION.env,
            capture_output=True, timeout=5)
        return result.returncode == 0 and result.stdout == data
    gui.SESSION.until('owned synthetic clipboard payload offered', offered)


def _refresh_source(ctx, index):
    window = ctx['window']
    point = upload.paperclip(window, ctx['preferences'], ctx['args'].zoom)
    popup = gui.open_native_popup(window, lambda: gui.click(window, *point))
    gui.capture_native_popup(window, popup, ctx['destination'], 'refresh-upload-sources')
    gui.popup_keys(window, popup, 'Home', *(['Down'] * index), 'Return')


def _refresh_host(ctx, activate=None, *, popup=None):
    window = ctx['window']
    if popup is None:
        popup = gui.open_native_popup(window, activate)
    gui.capture_native_popup(window, popup, ctx['destination'], 'refresh-upload-host-choice')
    # Existing ordered host list: custom is seventh. No public provider is contacted.
    gui.popup_keys(window, popup, 'Home', *(['Down'] * 6), 'Return')


def _refresh_no_autosend(ctx, url):
    gui.save_state(ctx['proc'], ctx['preferences'], ctx['database_path'])
    upload.assert_no_chat_url(ctx['database_path'], url)
    observer = ctx['observer']
    start = len(observer.frames)
    token = f'astra-upload-quiet-{time.monotonic_ns()}'
    # Drain through a positive server barrier, never time out the buffered reader.
    observer.send('PING :' + token)
    observer.wait(lambda command, params, prefix:
        command == 'PONG' and params and params[-1] == token, timeout=15)
    sent = [frame for frame in observer.frames[start:]
            if frame['command'] == 'PRIVMSG' and
            frame['prefix'].split('!', 1)[0] in {ctx['own_nick'], ctx['other_nick']}]
    assert not sent, ('upload unexpectedly sent an IRC PRIVMSG', sent)
    return {'inserted_url': url, 'native_composer_text': gui.native_input_state(ctx['window'])['text'],
            'no_chat_url_in_database': True, 'no_own_privmsg_on_wire': True}


def _refresh_multipart(fixture, payload, mime):
    path, headers, body = fixture.requests.get(timeout=20)
    assert path == '/upload'
    assert headers.get('Authorization') == 'Basic ' + base64.b64encode(
        b'gui-upload:private-upload-password').decode(), 'private upload auth differs'
    message = BytesParser(policy=default).parsebytes(
        b'Content-Type: ' + headers['Content-Type'].encode() + b'\r\nMIME-Version: 1.0\r\n\r\n' + body)
    assert message.is_multipart()
    parts = {part.get_param('name', header='content-disposition'): part for part in message.iter_parts()}
    assert parts['file'].get_payload(decode=True) == payload, 'consumer uploaded different snapshot bytes'
    assert parts['file'].get_content_type() == mime, 'snapshot MIME lost before transport'
    assert parts['file'].get_filename(), 'snapshot filename missing'
    assert parts['expires'].get_payload(decode=True) == b'12'
    return {'mime': parts['file'].get_content_type(), 'payload_bytes': len(payload),
            'filename': parts['file'].get_filename(),
            'multipart_body_bytes': len(body), 'exact_snapshot_bytes_verified': True,
            'private_tls_auth_verified': True}

def _refresh_upload_region(ctx):
    # Only a locator region: review can grow for a real image or retained text.
    # Native control/caret and raster observations prove the actual content.
    return gui.read_layout(ctx['preferences'])['conversation']['rect']


def _refresh_upload_action(ctx, text):
    upload.click_label(ctx['window'], ctx['destination'], text, region=_refresh_upload_region(ctx))



def _refresh_upload_image(ctx, label):
    """Two-color fixture pixels must appear in the actual local review."""
    def decoded():
        frame = gui.native_rgb_capture(ctx['window'])
        groups = [gui.native_marker_components(frame, _refresh_upload_region(ctx), rgb)
                  for rgb in ((231, 17, 193), (17, 193, 231))]
        if any(not items for items in groups):
            return False
        blocks = [max(items, key=lambda item: item['pixels']) for items in groups]
        if any(item['pixels'] <= 50 for item in blocks):
            return False
        a, b = (item['bbox'] for item in blocks)
        if not (a[2] <= b[0] and max(a[1], b[1]) < min(a[3], b[3])):
            return False
        upload_box = upload.label_rect(ctx['window'], ctx['destination'], 'Upload',
                                       region=_refresh_upload_region(ctx))
        return blocks if upload_box and max(a[3], b[3]) < upload_box[1] else False
    blocks = gui.SESSION.until('actual decoded local upload image before Upload', decoded)
    return {'decoded_color_blocks': blocks, 'capture': str(gui.capture_window(
        ctx['window'], Path(ctx['destination']), 'upload-preview-' + label))}


def run_upload(ctx):
    evidence = []
    fixture = ctx['cleanup'].enter_context(upload.UploadServer(ctx['lab']))
    upload.set_upload_settings(ctx['proc'], ctx['window'], ctx['preferences'],
        ctx['database_path'], ctx['destination'], fixture.endpoint)
    settings = tomllib.loads(Path(ctx['preferences']).read_text())['uploads']
    # Start with a different default, keeping the genuine private TLS credentials.
    settings['provider'] = 'x0at'
    import json
    import os
    portal, backend = os.environ.get('BLASTIRC_PORTAL'), os.environ.get('BLASTIRC_PORTAL_BACKEND')
    assert portal and backend, 'uploads requires pinned nix develop BLASTIRC_PORTAL/BLASTIRC_PORTAL_BACKEND'
    output = Path(ctx['destination']).with_name('refresh-private-upload-portals')
    output.mkdir(mode=0o700, exist_ok=True)
    previous_env = dict(ctx['env'])
    services = ctx['cleanup'].enter_context(
        upload.DesktopServices(ctx['env'], output, Path(portal), Path(backend)))
    ctx['cleanup'].callback(lambda: (ctx['env'].clear(), ctx['env'].update(previous_env)))
    services.start_portals()
    _refresh_restart(ctx, 'uploads', {key: json.dumps(value) if not isinstance(value, bool)
        else str(value).lower() for key, value in settings.items()})
    image_path = Path(ctx['destination']).with_name('synthetic-clipboard.png')
    pixels = Image.new('RGB', (96, 64), (231, 17, 193))
    pixels.paste((17, 193, 231), (48, 0, 96, 64))
    pixels.save(image_path)
    image_path.chmod(0o600)
    image = image_path.read_bytes()
    window = ctx['window']
    # Native paste from a NONEMPTY composer must bypass the empty-only accessory gate.
    draft = 'keep this draft '
    upload.focus_composer(window)
    gui.type_text(draft)
    _refresh_clipboard(ctx, image, 'image/png')
    gui.framed_keys(window, 'ctrl+v')
    gui.SESSION.until('image paste native review', lambda:
        upload.label_rect(window, ctx['destination'], 'Upload', region=_refresh_upload_region(ctx)))
    assert fixture.requests.empty(), 'image paste auto-uploaded before review'
    pasted_preview = _refresh_upload_image(ctx, 'clipboard')
    # This is the only Choose control in the panel; do not OCR-pin its ellipsis.
    _refresh_host(ctx, lambda: _refresh_upload_action(ctx, 'Choose'))
    assert tomllib.loads(Path(ctx['preferences']).read_text())['uploads']['provider'] == 'x0at', \
        'per-upload host choice silently changed default'
    _refresh_upload_action(ctx, 'Make default')
    gui.save_state(ctx['proc'], ctx['preferences'], ctx['database_path'])
    assert tomllib.loads(Path(ctx['preferences']).read_text())['uploads']['provider'] == 'custom'
    _refresh_upload_action(ctx, 'Upload')
    request = _refresh_multipart(fixture, image, 'image/png')
    url = fixture.endpoint.replace('/upload', '/file/pasted-image.png')
    fixture.responses.put((201, url))
    upload.focus_composer(window)
    inserted = draft + url
    gui.SESSION.until('image upload URL inserted into original nonempty draft', lambda:
        (gui.native_input_state(window) or {}).get('text') == inserted)
    gui._composer_native(window, inserted, len(inserted.encode()), len(inserted.encode()))
    outcome = _refresh_no_autosend(ctx, url)
    evidence.append({'source': 'nonempty-composer-image-paste', **request, **outcome,
                     'default_before': 'x0at', 'explicit_default_after': 'custom'})
    gui.command('undo', window=window)
    gui._composer_native(window, draft, len(draft), len(draft))
    gui.clear_draft()
    # SourceClipboard must also consume the offered image through the source chooser.
    _refresh_clipboard(ctx, image, 'image/png')
    _refresh_source(ctx, 2)
    gui.SESSION.until('SourceClipboard image review', lambda:
        upload.label_rect(window, ctx['destination'], 'Upload', region=_refresh_upload_region(ctx)))
    assert fixture.requests.empty(), 'SourceClipboard uploaded without explicit confirmation'
    source_preview = _refresh_upload_image(ctx, 'source-clipboard')
    _refresh_upload_action(ctx, 'Upload')
    request = _refresh_multipart(fixture, image, 'image/png')
    url = fixture.endpoint.replace('/upload', '/file/source-clipboard.png')
    fixture.responses.put((201, url))
    upload.focus_composer(window)
    gui.SESSION.until('SourceClipboard URL inserted', lambda:
        (gui.native_input_state(window) or {}).get('text') == url)
    outcome = _refresh_no_autosend(ctx, url)
    evidence.append({'source': 'SourceClipboard-image', **request, **outcome})
    gui.clear_draft()
    composer_before_text = gui._composer_native(window, '', 0, 0)
    # Text / paste is the genuine native editor -> review -> HTTPS upload consumer.
    _refresh_source(ctx, 1)
    lines = ['Synthetic text upload café 猫 ' + ('wrapped content ' * 9),
             'second line e\u0301', *('retained line ' + str(i) for i in range(14))]
    text = '\n'.join(lines)
    gui.SESSION.until('native text upload editor', lambda:
        upload.label_rect(window, ctx['destination'], 'Review text', region=_refresh_upload_region(ctx)))
    _, action_top, _, _ = upload.label_rect(window, ctx['destination'], 'Review text',
        region=_refresh_upload_region(ctx))
    panel_x, panel_y, panel_width, panel_height = _refresh_upload_region(ctx)
    def upload_editor_state(expected):
        state = gui.native_input_state(window)
        if not state or state['text'] != expected or not state['caret']:
            return False
        x, y, width, height = state['caret']
        inside = (panel_x <= x < x + width <= panel_x + panel_width
                  and panel_y <= y < y + height < min(action_top, panel_y + panel_height))
        return state if inside and state['caret'] != composer_before_text['caret'] else False
    # Pointer release clears button focus in the active SDK. Walk real
    # TabPrevious targets until the panel Input, not the composer, owns the caret.
    editor_before_text = upload_editor_state('')
    for _ in range(64):
        if editor_before_text:
            break
        gui.framed_keys(window, 'shift+Tab')
        editor_before_text = upload_editor_state('')
    assert editor_before_text, 'native text upload Input never acquired the caret'
    for index, line in enumerate(lines):
        if index:
            gui.framed_keys(window, 'Return')
        gui.type_text(line)
    editor_with_text = gui.SESSION.until('actual native upload editor contains synthetic text',
        lambda: upload_editor_state(text))
    assert fixture.requests.empty(), 'text editor started an upload'
    # More than one caret line is visible in the genuinely sized editor; after
    # overflow, navigation must scroll retained bytes rather than submitting.
    gui.framed_keys(window, 'ctrl+Home')
    first_line_state = gui.SESSION.until('multiline upload first line caret',
        lambda: state if (state := upload_editor_state(text)) and state['cursor'] == 0 else None)
    gui.framed_keys(window, 'Down')
    wrapped_line_state = gui.SESSION.until('upload native wrapped next visual line',
        lambda: state if (state := upload_editor_state(text)) and state['cursor'] > 0
        and state['caret'][1] > first_line_state['caret'][1] else None)
    gui.framed_keys(window, 'ctrl+End')
    editor_before_host = gui.SESSION.until('multiline upload scrolls to retained end',
        lambda: state if (state := upload_editor_state(text))
        and state['cursor'] == len(text.encode()) else None)
    _refresh_host(ctx, lambda: _refresh_upload_action(ctx, 'Choose'))
    # The popup returns focus to Choose host, not to the retained textarea.
    # Click its measured end caret; do not edit/retype to recover native focus.
    caret_x, caret_y, caret_width, caret_height = editor_before_host['caret']
    marker = gui.GUI_DEBUG[window][1].stat().st_size
    gui.click(window, caret_x + caret_width / 2, caret_y + caret_height / 2)
    gui.await_native_frame(window, marker, re.compile(r"wl_pointer[@#]\d+\.button\(\d+,\s*\d+,\s*272,\s*0\)"))
    editor_after_host = gui.SESSION.until('host choice refocuses retained native upload editor',
        lambda: upload_editor_state(text))
    assert (editor_after_host['text'].encode(), editor_after_host['cursor'], editor_after_host['anchor']) == (
        editor_before_host['text'].encode(), editor_before_host['cursor'], editor_before_host['anchor']), \
        ('Host choice changed native upload bytes or selection', editor_before_host, editor_after_host)
    _refresh_upload_action(ctx, 'Review text')
    assert fixture.requests.empty(), 'text review started an upload'
    text_review_capture = str(gui.capture_window(window, Path(ctx['destination']), 'upload-text-review'))
    _refresh_upload_action(ctx, 'Upload')
    request = _refresh_multipart(fixture, text.encode(), 'text/plain')
    url = fixture.endpoint.replace('/upload', '/file/native-text.txt')
    fixture.responses.put((201, url))
    upload.focus_composer(window)
    gui.SESSION.until('text upload URL inserted without send', lambda:
        (gui.native_input_state(window) or {}).get('text') == url)
    outcome = _refresh_no_autosend(ctx, url)
    evidence.append({'source': 'native-text-editor', 'editor_before': editor_before_text,
                     'editor_with_text': editor_with_text, 'first_line': first_line_state,
                     'wrapped_line': wrapped_line_state, 'review_capture': text_review_capture,
                     'before_host': editor_before_host, 'after_host': editor_after_host,
                     **request, **outcome})
    gui.command('undo', window=window)
    gui._composer_native(window, '', 0, 0)
    # Reuse the genuine private GTK portal for both supported image and honest
    # metadata-only file review. No portal response or controller model is injected.
    file_path = Path(ctx['destination']).with_name('synthetic-upload.txt')
    file_payload = 'Local generic file café e\u0301 猫\nNo image data.\n'.encode('utf-8')
    file_path.write_bytes(file_payload)
    file_path.chmod(0o600)
    for selected_file, payload, mime in ((image_path, image, 'image/png'),
                                        (file_path, file_payload, 'text/plain')):
        _refresh_source(ctx, 0)
        upload.native_choose(window, selected_file, Path(ctx['destination']), services)
        assert fixture.requests.empty(), 'File chooser uploaded before explicit Upload'
        if mime == 'image/png':
            review = _refresh_upload_image(ctx, 'native-file-picker')
        else:
            review = {'capture': str(gui.capture_window(
                window, Path(ctx['destination']), 'upload-generic-file-metadata'))}
            frame = gui.native_rgb_capture(window)
            assert not gui.native_marker_components(frame, _refresh_upload_region(ctx), (231, 17, 193)), \
                'Generic file retained the previous image preview instead of honest metadata'
        _refresh_upload_action(ctx, 'Upload')
        request = _refresh_multipart(fixture, payload, mime)
        assert request['filename'] == selected_file.name, 'Native picker upload lost the selected filename'
        url = fixture.endpoint.replace('/upload', '/file/' + selected_file.name)
        fixture.responses.put((201, url))
        upload.focus_composer(window)
        gui.SESSION.until('native file upload inserts URL without sending',
            lambda: (gui.native_input_state(window) or {}).get('text') == url)
        outcome = _refresh_no_autosend(ctx, url)
        evidence.append({'source': 'native-file-picker', 'selected_file': selected_file.name,
                         'preview': review, **request, **outcome})
        gui.command('undo', window=window)
        gui._composer_native(window, '', 0, 0)
    evidence.append({'clipboard_image_preview': pasted_preview,
                     'source_clipboard_image_preview': source_preview})
    assert tomllib.loads(Path(ctx['preferences']).read_text())['uploads']['provider'] == 'custom'
    return _refresh_return(ctx, 'uploads', evidence)

def run_uploads(ctx):
    """Plural entry point used by the parent's --scope uploads dispatcher."""
    return run_upload(ctx)



class _RefreshMediaOrigin:
    def __init__(self, lab, assets):
        self.assets, self.requests, self.lock = assets, [], threading.Lock()
        fixture = self
        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass
            def do_GET(self):
                with fixture.lock:
                    fixture.requests.append((self.path, dict(self.headers)))
                asset = fixture.assets.get(self.path)
                if asset is None:
                    self.send_error(404)
                    return
                data, mime, *status = asset
                self.send_response(status[0] if status else 200)
                self.send_header('Content-Type', mime)
                self.send_header('Content-Length', str(len(data)))
                self.end_headers()
                self.wfile.write(data)
        self.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.server.daemon_threads = True
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(lab.root / 'server.pem', lab.root / 'server.key')
        self.server.socket = tls.wrap_socket(self.server.socket, server_side=True)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.url = f'https://localhost:{self.server.server_port}'
    def __enter__(self):
        self.thread.start()
        return self
    def __exit__(self, *exc):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)
    def snapshot(self):
        with self.lock:
            return list(self.requests)


def _refresh_overlay(ctx):
    geometry = gui.window_geometry(ctx['window'])
    scale = ctx['args'].zoom
    width, height = geometry['WIDTH'], geometry['HEIGHT']
    margin = min(16 * scale, width / 8, height / 8)
    w, h = min(1000 * scale, width - 2 * margin), min(740 * scale, height - 2 * margin)
    return (width - w) / 2, (height - h) / 2, w, h, scale


def _refresh_pixels(ctx, name):
    path = Path(ctx['destination']).with_name('refresh-media-' + name + '.png')
    # Cursor pixels are not decoded-frame content. Keep the same safe pointer
    # location for initial, playing, paused, seek and reopened-frame comparisons.
    gui.move_pointer(ctx['window'], 8, 8)
    width, height = gui.display_geometry()
    gui.capture_png(gui.SESSION.env, path, scale=1, expected_pixels=(width, height), output='HEADLESS-1')
    geometry = gui.window_geometry(ctx['window'])
    x, y, w, h, scale = _refresh_overlay(ctx)
    # Interior image only: exclude title, status, controls and pointer hover effects.
    box = (geometry['X'] + x + w * .25, geometry['Y'] + y + 100 * scale,
           geometry['X'] + x + w * .75, geometry['Y'] + y + h - 150 * scale)
    with Image.open(path) as screenshot:
        return screenshot.convert('RGB').crop(tuple(round(v) for v in box))


def _refresh_frame_hash(image):
    import hashlib
    return hashlib.sha256(image.tobytes()).hexdigest()


def _refresh_changed_pixels(first, second):
    assert first.size == second.size, 'native video viewport geometry changed during observation'
    return sum(max(pixel) > 20
               for pixel in ImageChops.difference(first, second).get_flattened_data())


def _refresh_decoded_video(ctx, name):
    image = _refresh_pixels(ctx, name)
    # The owned testsrc2 fixture contains vivid color bars; the empty native
    # surface does not. Wait for a real decoded frame, not an arbitrary delay.
    colorful = sum(max(pixel) - min(pixel) > 80 for pixel in image.get_flattened_data())
    return image if colorful > image.width * image.height * .2 else None


def _refresh_volume_pixels(ctx, name):
    x, y, w, h, scale = _refresh_overlay(ctx)
    geometry = gui.window_geometry(ctx['window'])
    left, top = geometry['X'] + x + w - 152 * scale, geometry['Y'] + y + h - 48 * scale
    path = Path(ctx['destination']).with_name('refresh-volume-' + name + '.png')
    gui.move_pointer(ctx['window'], 8, 8)
    gui.capture_png(gui.SESSION.env, path, scale=1, expected_pixels=(140, 38),
                    region=f'{round(left)},{round(top)} 140x38')
    with Image.open(path) as captured:
        return captured.convert('RGB').copy(), str(path)


def _refresh_volume_thumb(image, color):
    # SDK thumb extends above the 8px track: row 13 contains the round
    # 18px thumb but no fill/track. Match its own observed baseline color.
    matches = [x for x in range(image.width)
               if max(abs(a - b) for a, b in zip(image.getpixel((x, 13)), color)) < 25]
    if not 4 <= len(matches) <= 20:
        return None
    return sum(matches) / len(matches)


def _refresh_clock(ctx):
    x, y, w, h, scale = _refresh_overlay(ctx)
    # The status is one native text line below the video, not a page/block.
    words = upload.visible_words(ctx['window'], ctx['destination'], 'media-clock',
        region=(x + 8 * scale, y + h - 116 * scale,
                min(w - 16 * scale, 300 * scale), 28 * scale), psm=7)
    text = ' '.join(word['text'] for word in words)
    match = re.search(r'(\d+):([0-5]\d)\s*/\s*(\d+):([0-5]\d)', text)
    if not match:
        return None
    a, b, c, d = map(int, match.groups())
    return a * 60 + b, c * 60 + d, text


def _refresh_media_close(ctx):
    x, y, w, h, scale = _refresh_overlay(ctx)
    return upload.label_rect(ctx['window'], ctx['destination'], 'Close',
        region=(x + w - 100 * scale, y, 100 * scale, 64 * scale))


def run_media(ctx):
    import os
    import sys
    private = Path(ctx['destination']).with_name(Path(ctx['destination']).stem + '-media-link-exec')
    private.mkdir(mode=0o700, exist_ok=False)
    calls = private / 'calls.jsonl'
    opener = private / 'xdg-open'
    opener.write_text('#!' + sys.executable + '\n'
        'import json, os, sys\n'
        'fd = os.open(' + repr(str(calls)) + ', os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)\n'
        'os.write(fd, (json.dumps(sys.argv[1:]) + "\\n").encode())\n'
        'os.close(fd)\n')
    opener.chmod(0o700)
    previous_env = dict(ctx['env'])
    ctx['env'] = dict(previous_env, PATH=str(private) + os.pathsep + previous_env['PATH'])
    ctx['cleanup'].callback(lambda: ctx.update(env=previous_env))
    expected_launches = []
    # The owned compositor runtime has no physical audio server. Decode through
    # OpenAL's null device only in this synthetic fixture, never in production.
    ctx['env'] = dict(ctx['env'], ALSOFT_DRIVERS='null')
    _refresh_restart(ctx, 'previews', dict(links='"click"', images='"click"', media='"click"',
        allow_local='true', max_height='120', media_max_mib='16', media_max_seconds='60'))
    root = Path(ctx['destination']).parent
    image_path, audio_path, video_path = (root / ('synthetic-preview' + ext) for ext in ('.png', '.wav', '.mp4'))
    Image.new('RGB', (640, 360), (231, 17, 193)).save(image_path)
    gif_path = root / 'synthetic-preview-animation.gif'
    Image.new('RGB', (640, 360), (231, 81, 17)).save(gif_path, save_all=True,
        append_images=[Image.new('RGB', (640, 360), (17, 231, 53))],
        duration=[100, 100], loop=0, disposal=2)
    subprocess.run(['ffmpeg', '-nostdin', '-v', 'error', '-y', '-f', 'lavfi', '-i',
        'sine=frequency=440:duration=12', '-c:a', 'pcm_s16le', str(audio_path)],
        env=ctx['env'], check=True, timeout=30, stdout=subprocess.DEVNULL)
    subprocess.run(['ffmpeg', '-nostdin', '-v', 'error', '-y', '-f', 'lavfi', '-i',
        'testsrc2=size=320x180:rate=12:duration=12', '-c:v', 'mpeg4', '-q:v', '3', '-pix_fmt', 'yuv420p',
        str(video_path)], env=ctx['env'], check=True, timeout=30, stdout=subprocess.DEVNULL)
    for path in (image_path, audio_path, video_path, gif_path):
        path.chmod(0o600)
    origin = ctx['cleanup'].enter_context(_RefreshMediaOrigin(ctx['lab'], {
        '/image.png': (image_path.read_bytes(), 'image/png'),
        '/animation.gif': (gif_path.read_bytes(), 'image/gif'),
        '/page': (b'<!doctype html><html><head><title>Native HTML Metadata</title>'
                  b'<meta name="description" content="Private generic page description">'
                  b'</head><body>Owned preview page</body></html>', 'text/html'),
        '/denied': (b'<html><title>RefusedPageMustNotRender</title></html>', 'text/html', 403),
        '/failed': (b'Owned failed metadata response', 'text/html', 500),
        '/automatic-page': (b'<html><head><title>Owned thumbnail control</title>'
                            b'<meta property="og:image" content="/automatic-thumbnail.png">'
                            b'</head></html>', 'text/html'),
        '/automatic-thumbnail.png': (image_path.read_bytes(), 'image/png'),
        '/automatic-noimage': (b'<html><title>No thumbnail</title></html>', 'text/html'),
        '/automatic-broken': (b'<html><head><meta property="og:image" content="/broken.png">'
                              b'</head></html>', 'text/html'),
        '/broken.png': (b'Not a decoded image', 'image/png'),
        '/off-page': (b'<html>Must never be requested</html>', 'text/html'),
        '/audio.wav': (audio_path.read_bytes(), 'audio/wav'),
        '/video.mp4': (video_path.read_bytes(), 'video/mp4')}))
    window = ctx['window']
    evidence = []
    def caption(label, top, expected_url):
        from difflib import SequenceMatcher
        from urllib.parse import urlsplit
        upload.focus_composer(window)
        x, _, width, _ = gui.read_layout(ctx['preferences'])['conversation']['rect']
        bottom = gui.native_input_rectangle(window)[1]
        assert top < bottom, ('No visible caption band below thumbnail', top, bottom)
        before_caption = origin.snapshot()
        rows = upload.visible_words(window, Path(ctx['destination']), 'media-caption-' + label,
                                    region=(x, top, width, bottom - top), psm=11)
        text = ''.join(row['text'] for row in rows).casefold()
        compact = lambda value: re.sub('[^a-z0-9]', '', value.casefold())
        assert urlsplit(expected_url).hostname in text and SequenceMatcher(
            None, compact(expected_url), compact(text)).ratio() >= .75, (
                'Native thumbnail caption is not its original URL fallback', expected_url, rows)
        assert origin.snapshot() == before_caption, 'Reading the native caption fetched its origin'
        return {'expected_url': expected_url, 'words': rows, 'region': (x, top, width, bottom - top)}

    for name in ('image.png', 'animation.gif', 'audio.wav', 'video.mp4'):
        before = origin.snapshot()
        text = 'Synthetic private preview ' + origin.url + '/' + name
        ctx['observer'].send(f"PRIVMSG {ctx['channel']} :{text}")
        ctx['expected_messages'].extend(ctx['channel_messages'](text, ctx['query_target']))
        gui.wait_messages(ctx['proc'], ctx['database_path'], ctx['expected_messages'])
        upload.focus_composer(window)
        load_rect = gui.SESSION.until('native unloaded preview card', lambda:
            upload.label_rect(window, ctx['destination'], 'Load preview'))
        # Consent sits inside the thumbnail; keep its actual center for cached
        # reopens rather than depending on a separate View-image button.
        action_x, action_y, action_width, action_height = load_rect
        thumbnail_point = (action_x + action_width / 2, action_y + action_height / 2)
        time.sleep(.6)
        assert origin.snapshot() == before, 'click privacy fetched private media before explicit load'
        upload.click_label(window, ctx['destination'], 'Load preview')
        gui.SESSION.until('native in-app preview Close action for ' + name,
            lambda: _refresh_media_close(ctx))
        requested = gui.SESSION.until('actual private preview origin receives request',
            lambda: origin.snapshot()[len(before):])
        assert requested and all(path == '/' + name for path, headers in requested), requested
        assert all(not any(key.lower() in ('authorization', 'cookie', 'proxy-authorization')
            for key in headers) for path, headers in requested), 'preview leaked ambient credentials'
        observed = {'asset': name, 'request_paths': [path for path, headers in requested],
                    'requests_before_click': 0, 'anonymous_headers_verified': True}
        if name in ('image.png', 'animation.gif'):
            first_rgb = (231, 17, 193) if name == 'image.png' else (231, 81, 17)
            def decoded_image():
                pixels = _refresh_pixels(ctx, 'decoded-image')
                count = sum(max(abs(a - b) for a, b in zip(pixel, first_rgb)) <= 2
                            for pixel in pixels.get_flattened_data())
                return count if count > 20000 else None
            matching = gui.SESSION.until(
                'native image popup renders actual decoded fixture pixels', decoded_image)
            observed['decoded_image_matching_pixels'] = matching
            if name == 'animation.gif':
                started, samples = time.monotonic(), []
                while len(samples) < 2 or time.monotonic() - started < .8:
                    pixels = _refresh_pixels(ctx, 'gif-static-first-frame')
                    first = sum(max(abs(a - b) for a, b in zip(pixel, first_rgb)) <= 2
                                for pixel in pixels.get_flattened_data())
                    second = sum(g > 200 and r < 45 and b < 90 for r, g, b in pixels.get_flattened_data())
                    assert first > 20000 and second == 0, (
                        'GIF did not stay on its actual first decoded frame', first, second)
                    samples.append({'elapsed': time.monotonic() - started,
                                    'first_color_pixels': first, 'second_color_pixels': second})
                observed['static_gif_first_frame_samples'] = samples
            gui.framed_keys(window, 'Escape')
            def loaded_thumbnail():
                frame = gui.native_rgb_capture(window)
                items = gui.native_marker_components(frame,
                    gui.read_layout(ctx['preferences'])['conversation']['rect'], first_rgb)
                full = [item['bbox'] for item in items if item['full']]
                return max(full, key=lambda box: (box[2] - box[0]) * (box[3] - box[1])) if full else None
            thumbnail_box = gui.SESSION.until('decoded image thumbnail in transcript', loaded_thumbnail)
            observed['thumbnail_box'] = thumbnail_box
            thumbnail_point = ((thumbnail_box[0] + thumbnail_box[2]) / 2,
                               (thumbnail_box[1] + thumbnail_box[3]) / 2)
            gui.click(window, *thumbnail_point)
            gui.SESSION.until('cached thumbnail opens viewer', lambda: _refresh_media_close(ctx))
            gui.SESSION.until('cached thumbnail opens actual image raster', decoded_image)
            gui.framed_keys(window, 'Escape')
        else:
            clock = gui.SESSION.until('decoded ' + name + ' duration in native player', lambda:
                (value if value and 11 <= value[1] <= 13 else None)
                if (value := _refresh_clock(ctx)) else None)
            assert clock[0] <= 1, 'player autoplayed before Play'
            observed['decoded_duration_seconds'] = clock[1]
            observed['initial_clock'] = clock
            video = name == 'video.mp4'
            if video:
                initial_frame = gui.SESSION.until('first decoded native video frame', lambda:
                    _refresh_decoded_video(ctx, 'video-initial'))
                initial_hash = _refresh_frame_hash(initial_frame)
                # Observe the same paused interval used for cached reopens.
                time.sleep(1.1)
                initial_after = _refresh_pixels(ctx, 'video-before-play')
                assert _refresh_frame_hash(initial_after) == initial_hash, 'video autoplayed before Play'
                observed['initial_video_frame_hash'] = initial_hash
                observed['no_autoplay_frame_hash'] = _refresh_frame_hash(initial_after)
            upload.click_label(window, ctx['destination'], 'Play')
            if video:
                def moving_video():
                    image = _refresh_pixels(ctx, 'decoded-video-first')
                    changed = _refresh_changed_pixels(initial_frame, image)
                    return image if changed > image.width * image.height * .01 else None
                first = gui.SESSION.until('Play changes actual decoded video pixels', moving_video)
                time.sleep(.7)
                second = _refresh_pixels(ctx, 'decoded-video-second')
                changed = _refresh_changed_pixels(first, second)
                assert changed > first.width * first.height * .01, 'decoded native video frame did not change'
                observed['changed_video_pixels'] = changed
                observed['sampled_video_pixels'] = first.width * first.height
            else:
                advanced = gui.SESSION.until('real decoded playback time advances', lambda:
                    (value if value and value[0] >= 2 else None)
                    if (value := _refresh_clock(ctx)) else None)
                observed['advanced_clock'] = advanced
            marker = gui.GUI_DEBUG[window][1].stat().st_size
            upload.click_label(window, ctx['destination'], 'Pause')
            gui.await_native_frame(window, marker, re.compile(r"wl_pointer[@#]\d+\.button\("))
            if video:
                paused_frame = _refresh_pixels(ctx, 'video-paused')
                paused_hash = _refresh_frame_hash(paused_frame)
                time.sleep(1.2)
                paused_after = _refresh_pixels(ctx, 'video-paused-after-wait')
                assert _refresh_frame_hash(paused_after) == paused_hash, 'Pause did not stop decoded video frames'
                observed['paused_video_frame_hash'] = paused_hash
                observed['paused_after_wait_frame_hash'] = _refresh_frame_hash(paused_after)
            else:
                paused = gui.SESSION.until('paused native player clock', lambda: _refresh_clock(ctx))
                time.sleep(1.2)
                paused_after = _refresh_clock(ctx)
                assert paused_after[0] == paused[0], 'pause did not stop actual playback position'
                observed['paused_clock'] = paused
                observed['paused_after_wait_clock'] = paused_after
            x, y, w, h, scale = _refresh_overlay(ctx)
            pad, inner = 12 * scale, w - 24 * scale
            marker = gui.GUI_DEBUG[window][1].stat().st_size
            gui.click(window, x + pad + 92 * scale + (inner - 250 * scale) * .7,
                      y + h - 32 * scale)
            gui.await_native_frame(window, marker, re.compile(r"wl_pointer[@#]\d+\.button\("))
            if video:
                def sought_video():
                    image = _refresh_pixels(ctx, 'video-seek')
                    changed = _refresh_changed_pixels(paused_frame, image)
                    return (image, changed) if changed > image.width * image.height * .01 else None
                sought_frame, sought_pixels = gui.SESSION.until(
                    'seek changes a paused decoded video frame', sought_video)
                observed['sought_video_frame_hash'] = _refresh_frame_hash(sought_frame)
                observed['seek_changed_video_pixels'] = sought_pixels
            else:
                sought = gui.SESSION.until('native seek changes decoded player position', lambda:
                    (value if value and 6 <= value[0] <= 10 else None)
                    if (value := _refresh_clock(ctx)) else None)
                observed['sought_clock'] = sought
            # Each media asset may retain the prior player's volume. Establish
            # maximum through the real slider before checking the quarter click.
            marker = gui.GUI_DEBUG[window][1].stat().st_size
            gui.click(window, x + w - pad - 2 * scale, y + h - 32 * scale)
            gui.await_native_frame(window, marker, re.compile(r"wl_pointer[@#]\d+\.button\("))
            def volume_maximum():
                image, path = _refresh_volume_pixels(ctx, name + '-before')
                color = image.getpixel((129, 13))
                position = _refresh_volume_thumb(image, color)
                return (position, path, color) if position is not None and 122 <= position <= 134 else None
            before_thumb, volume_before_path, thumb_color = gui.SESSION.until(
                'native volume thumb visibly reaches the maximum endpoint', volume_maximum)
            gui.click(window, x + w - pad - 140 * scale + 35 * scale, y + h - 32 * scale)
            def volume_moved():
                image, path = _refresh_volume_pixels(ctx, name + '-quarter')
                position = _refresh_volume_thumb(image, thumb_color)
                return (position, path) if position is not None and 29 <= position <= 41 else None
            volume_thumb, volume_after_path = gui.SESSION.until(
                'native volume thumb visibly moves to quarter position', volume_moved)
            observed['volume_control'] = {'before_thumb_x': before_thumb, 'after_thumb_x': volume_thumb,
                'track_x': 2, 'track_width': 136, 'before_crop': volume_before_path,
                'after_crop': volume_after_path, 'assertion': 'native slider thumb at quarter position'}
            upload.click_label(window, ctx['destination'], 'Close')
            gui.SESSION.until('player overlay closed', lambda: not upload.label_rect(
                window, ctx['destination'], 'Play', region=(x, y + h - 60 * scale, w, 60 * scale)))
            # Reopening the cached asset must create paused playback at the start,
            # rather than hiding a still-running decoder/player behind the overlay.
            gui.click(window, *thumbnail_point)
            if video:
                def restarted_video():
                    image = _refresh_pixels(ctx, 'video-reopened')
                    return image if _refresh_frame_hash(image) == initial_hash else None
                reopened = gui.SESSION.until(
                    'reopened video renders its original decoded start frame', restarted_video)
                time.sleep(1.1)
                reopened_after = _refresh_pixels(ctx, 'video-reopened-after-wait')
                assert _refresh_frame_hash(reopened_after) == initial_hash, 'closed video resumed stale playback'
                observed['reopened_video_frame_hash'] = _refresh_frame_hash(reopened)
                observed['reopened_after_wait_frame_hash'] = _refresh_frame_hash(reopened_after)
            else:
                reopened = gui.SESSION.until('closed player reopened stopped at start', lambda:
                    (value if value and value[0] <= 1 and value[1] >= 11 else None)
                    if (value := _refresh_clock(ctx)) else None)
                time.sleep(1.1)
                reopened_after = _refresh_clock(ctx)
                assert reopened_after[0] <= 1, 'closed player resumed stale playback'
                observed['reopened_clock'] = reopened
                observed['reopened_after_wait_clock'] = reopened_after
            gui.framed_keys(window, 'Escape')
        # Info exposes selectable original URL/metadata without OCR prose pins.
        popup = gui.open_native_popup(window, lambda: (
            gui.move_pointer(window, *thumbnail_point),
            gui.POINTER.button(True, code=273), gui.POINTER.button(False, code=273)))
        gui.popup_keys(window, popup, 'Home', 'Return')
        info = gui.SESSION.until('native selectable preview Info',
            lambda: state if (state := gui.native_input_state(window))
            and state.get('text', '').splitlines()
            and state['text'].splitlines()[0] == origin.url + '/' + name else None)
        info_line_bytes = len(info['text'].splitlines()[0].encode('utf-8'))
        gui.framed_keys(window, 'ctrl+Home', 'Right', 'ctrl+e')
        info_eol = gui._composer_native(window, info['text'], info_line_bytes, info_line_bytes)
        observed['readonly_info_ctrl_e_line_end'] = info_eol
        gui.framed_keys(window, 'ctrl+a', 'ctrl+c')
        gui.SESSION.until('Info copies exact original URL and real metadata',
            lambda: gui.clipboard_text() == info['text'])
        observed['info_copied_text'] = gui.clipboard_text()
        if name in ('image.png', 'animation.gif'):
            assert '640' in info['text'] and '360' in info['text'], info['text']
        if name == 'animation.gif':
            assert 'Preview: static first frame' in info['text'], info['text']
        gui.framed_keys(window, 'Escape')
        observed['caption'] = caption(name,
            thumbnail_box[3] if name in ('image.png', 'animation.gif') else load_rect[1],
            origin.url + '/' + name)
        evidence.append(observed)
        upload.focus_composer(window)
        gui._composer_native(window, '', 0, 0)
    # The original URL, not a placeholder tile, owns HTML consent and copying.
    marker_serial = 0
    def html_rows(value, target):
        nonlocal marker_serial
        marker_serial += 1
        colors = [(40 + marker_serial * 9, 145, 83),
                  (145, 40 + marker_serial * 9, 183),
                  (193, 123, 40 + marker_serial * 9)]
        def marker(index):
            color = ''.join(f'{part:02X}' for part in colors[index])
            return '\x04' + color + ',' + color + 'MM\x0f'
        _policy_send(ctx, marker(0) + ' ' + value + ' ' + marker(1), target=target)
        _policy_send(ctx, marker(2) + ' Native HTML following row', target=target)
        upload.focus_composer(window)
        def boxes():
            frame = gui.native_rgb_capture(window)
            found = [_policy_box(ctx, color, image=frame) for color in colors]
            return found if all(found) else None
        found = gui.SESSION.until('native HTML source and following row markers', boxes)
        assert abs(found[0][1] - found[1][1]) <= 1, ('HTML source unexpectedly wrapped', found)
        return colors, found

    def html_boxes(colors):
        frame = gui.native_rgb_capture(window)
        boxes = [_policy_box(ctx, color, image=frame) for color in colors]
        assert all(boxes), ('HTML fixture source no longer visible', boxes)
        return boxes

    def no_html_card(colors, reference_gap):
        boxes = html_boxes(colors)
        gap = boxes[2][1] - boxes[0][1]
        assert abs(gap - reference_gap) <= 1, (
            'HTML without decoded thumbnail reserved a native card/gap', reference_gap, gap, boxes)
        return {'native_boxes': boxes, 'row_gap': gap, 'reference_row_gap': reference_gap}

    def url_menu(colors, label):
        left, right, _ = html_boxes(colors)
        point = ((left[2] + right[0]) / 2, (left[1] + left[3]) / 2)
        popup = gui.open_native_popup(window, lambda: (
            gui.move_pointer(window, *point),
            gui.POINTER.button(True, code=273), gui.POINTER.button(False, code=273)))
        popup['evidence_capture'] = str(gui.capture_native_popup(window, popup,
            ctx['destination'], 'html-url-menu-' + label))
        return popup

    def menu_action(popup, label):
        # Selected native rows invert their text colors; use the existing menu
        # order, and prove the actions through clipboard/HTTPS results, not OCR.
        keys = {'Copy text': (), 'Load preview': ('ctrl+n',)}[label]
        gui.popup_keys(window, popup, 'Home', *keys, 'Return', root_changed=False)

    def copy_url(colors, url, label):
        gui.seed_clipboard('native-html-copy-sentinel-' + label, ctx['cleanup'])
        popup = url_menu(colors, label)
        menu_action(popup, 'Copy text')
        gui.SESSION.until('native message copy retains exact original HTML URL bytes',
            lambda: [word.encode('utf-8') for word in gui.clipboard_text().split()
                     if word.startswith('https://')] == [url.encode('utf-8')])
        left, right, _ = html_boxes(colors)
        gui.click(window, (left[2] + right[0]) / 2, (left[1] + left[3]) / 2)
        expected_launches.append([url])
        gui.SESSION.until('original HTML URL reaches actual private xdg-open boundary',
            lambda: calls.exists() and [json.loads(line) for line in calls.read_text().splitlines()]
            == expected_launches)
        return {'copied_source_contains_original_url': url, 'opened_original_url': url,
                'native_menu_capture': popup['evidence_capture']}

    _, reference = html_rows('Native HTML plain reference', ctx['channel'])
    reference_gap = reference[2][1] - reference[0][1]
    for name in ('page', 'denied', 'failed'):
        before = origin.snapshot()
        url = origin.url + '/' + name
        colors, _ = html_rows(url, ctx['channel'])
        observed = {'asset': name, 'mode': 'click',
                    'before_consent': no_html_card(colors, reference_gap)}
        observed.update(copy_url(colors, url, name + '-before'))
        time.sleep(.6)
        assert origin.snapshot() == before, 'HTML Click policy fetched before explicit consent'
        popup = url_menu(colors, name + '-load')
        menu_action(popup, 'Load preview')
        gui.SESSION.until('owned HTML explicit request', lambda: origin.snapshot()[len(before):])
        # Permit the real worker completion to return to the native event loop.
        time.sleep(.6)
        gui.framed_keys(window, 'Escape')
        upload.focus_composer(window)
        observed['after_negative'] = no_html_card(colors, reference_gap)
        observed.update(copy_url(colors, url, name + '-negative'))
        gui.framed_keys(window, 'Tab', 'shift+Tab', 'Left', 'Right')
        time.sleep(.6)
        requests = origin.snapshot()[len(before):]
        assert [path for path, _ in requests] == ['/' + name], (
            'Negative HTML result retried or fetched an unauthorized destination', requests)
        assert all(not any(key.lower() in ('authorization', 'cookie', 'proxy-authorization')
                           for key in headers) for _, headers in requests), requests
        observed.update(request_paths=[path for path, _ in requests],
                        requests_before_click=0, anonymous_headers_verified=True,
                        negative_result_no_retry=True)
        evidence.append(observed)
    if 'draft/message-redaction' in ctx['observer'].capabilities:
        retirement = []
        for choice, action in enumerate(('Info', 'Copy Link')):
            url = origin.url + '/image.png?retirement=' + str(choice)
            text = 'Synthetic retired preview ' + url
            ctx['observer'].send(f"PRIVMSG {ctx['channel']} :{text}")
            # These temporary source rows are deliberately not added to the
            # final expected originals: the real server will redact them.
            gui.wait_messages(ctx['proc'], ctx['database_path'], ctx['expected_messages'] +
                ctx['channel_messages'](text, ctx['query_target']))
            with sqlite3.connect(f"file:{ctx['database_path']}?mode=ro", uri=True) as database:
                message_id, = database.execute(
                    'SELECT id FROM messages WHERE buffer=? AND text=?',
                    (ctx['channel_id'], text)).fetchone()
            assert message_id and not message_id.startswith('local:'), message_id
            upload.focus_composer(window)
            box = gui.SESSION.until('retirement source honest consent thumbnail',
                lambda: upload.label_rect(window, ctx['destination'], 'Load preview'))
            point = (box[0] + box[2] / 2, box[1] + box[3] / 2)
            sentinel = 'private-retired-preview-clipboard-' + str(choice)
            gui.seed_clipboard(sentinel, ctx['cleanup'])
            popup = gui.open_native_popup(window, lambda: (
                gui.move_pointer(window, *point),
                gui.POINTER.button(True, code=273), gui.POINTER.button(False, code=273)))
            marker = gui.GUI_DEBUG[window][1].stat().st_size
            requests = origin.snapshot()
            ctx['observer'].send(f"REDACT {ctx['channel']} {message_id}")
            gui.wait_rows(ctx['proc'], ctx['database_path'],
                'SELECT buffer,text,deleted FROM messages WHERE id=? ORDER BY buffer',
                (message_id,), sorted([(ctx['channel_id'], '', 1), (ctx['other_channel_id'], '', 1)]))
            with gui.GUI_DEBUG[window][1].open('rb') as trace:
                trace.seek(marker)
                retired = re.search(
                    rf"xdg_popup[@#]{popup['popup']}\.destroy\(".encode(), trace.read()) is not None
            if not retired:
                # Exercise the existing, now stale, native menu closure. A live
                # readability resolver must reject it rather than trust open-time state.
                gui.popup_keys(window, popup, 'Home', *(['Down'] * choice), 'Return',
                               root_changed=False)
            # Popup destruction precedes its deferred action. A real focus
            # round trip fences the callback without requiring a no-op action
            # itself to repaint or dismissing an incorrectly opened Info scene.
            gui.framed_keys(window, 'Tab', 'shift+Tab')
            current_input = gui.native_input_state(window)
            assert not current_input or url not in (current_input.get('text') or ''), \
                'Retired preview source leaked into a stale Info textbox'
            assert gui.clipboard_text() == sentinel, 'Retired preview source copied its URL'
            assert origin.snapshot() == requests, 'Retired preview context fetched its source'
            retirement.append({'action': action, 'wire_redacted_id': message_id,
                               'native_menu_retired': retired, 'stale_action_denied': not retired,
                               'clipboard_unchanged': True,
                               'capture': str(gui.capture_window(window, Path(ctx['destination']),
                                   'media-retired-' + str(choice)))})
            upload.focus_composer(window)
            gui._composer_native(window, '', 0, 0)
        evidence.append({'actual_wire_source_retirement': retirement})
    else:
        evidence.append({'actual_wire_source_retirement': 'not exercised',
                         'limitation': 'Private IRC server does not advertise draft/message-redaction; '
                                       'no injected source retirement substitutes for this flow.'})
    # Use the existing quiet query for policy restarts: old direct-media rows
    # must not become an unrelated Automatic network workload.
    _policy_focus(ctx, ctx['query_id'])
    _refresh_restart(ctx, 'previews', dict(links='"automatic"', images='"automatic"', media='"click"',
        allow_local='true', max_height='120', media_max_mib='16', media_max_seconds='60'),
        focus=ctx['query_id'])
    window = ctx['window']
    _, reference = html_rows('Native automatic plain reference', ctx['own_nick'])
    reference_gap = reference[2][1] - reference[0][1]
    before = origin.snapshot()
    url = origin.url + '/automatic-page'
    colors, _ = html_rows(url, ctx['own_nick'])
    def decoded_html_thumbnail():
        frame = gui.native_rgb_capture(window)
        items = gui.native_marker_components(frame,
            gui.read_layout(ctx['preferences'])['conversation']['rect'], (231, 17, 193))
        full = [item['bbox'] for item in items if item['full']]
        if not full:
            return None
        box = max(full, key=lambda item: (item[2] - item[0]) * (item[3] - item[1]))
        pixels = _policy_crop(frame, box)
        matching = sum(max(abs(pixels[index + channel] - color)
                           for channel, color in enumerate((231, 17, 193))) <= 2
                       for index in range(0, len(pixels), 3))
        return {'thumbnail_box': box, 'decoded_magenta_pixels': matching} if matching > 10000 else None
    decoded = gui.SESSION.until('HTML og:image paints actual decoded magenta thumbnail',
                                decoded_html_thumbnail)
    decoded.update(copy_url(colors, url, 'automatic-valid'))
    time.sleep(.6)
    requests = origin.snapshot()[len(before):]
    assert [path for path, _ in requests] == ['/automatic-page', '/automatic-thumbnail.png'], requests
    assert all(not any(key.lower() in ('authorization', 'cookie', 'proxy-authorization')
                       for key in headers) for _, headers in requests), requests
    evidence.append(dict(decoded, asset='automatic-page', mode='automatic',
                         request_paths=[path for path, _ in requests], anonymous_headers_verified=True))
    for name, paths in (('automatic-noimage', ['/automatic-noimage']),
                        ('automatic-broken', ['/automatic-broken', '/broken.png'])):
        before = origin.snapshot()
        url = origin.url + '/' + name
        colors, _ = html_rows(url, ctx['own_nick'])
        gui.SESSION.until('owned automatic negative HTML request chain', lambda:
            len(origin.snapshot()[len(before):]) >= len(paths))
        time.sleep(.6)
        observed = no_html_card(colors, reference_gap)
        observed.update(copy_url(colors, url, name))
        gui.framed_keys(window, 'Tab', 'shift+Tab', 'Left', 'Right')
        time.sleep(.6)
        requests = origin.snapshot()[len(before):]
        assert [path for path, _ in requests] == paths, ('Automatic negative retried', requests)
        assert all(not any(key.lower() in ('authorization', 'cookie', 'proxy-authorization')
                           for key in headers) for _, headers in requests), requests
        evidence.append(dict(observed, asset=name, mode='automatic', request_paths=paths,
                             anonymous_headers_verified=True, negative_result_no_retry=True))
    _refresh_restart(ctx, 'previews', dict(links='"off"', images='"off"', media='"click"',
        allow_local='true', max_height='120', media_max_mib='16', media_max_seconds='60'),
        focus=ctx['query_id'])
    window = ctx['window']
    _, reference = html_rows('Native disabled plain reference', ctx['own_nick'])
    reference_gap = reference[2][1] - reference[0][1]
    before = origin.snapshot()
    url = origin.url + '/off-page'
    colors, _ = html_rows(url, ctx['own_nick'])
    observed = no_html_card(colors, reference_gap)
    observed.update(copy_url(colors, url, 'off'))
    gui.framed_keys(window, 'Tab', 'shift+Tab', 'Left', 'Right')
    time.sleep(.6)
    assert origin.snapshot() == before, 'Off HTML policy made a preview request'
    evidence.append(dict(observed, asset='off-page', mode='off',
                         request_paths=[], native_context_opened_without_request=True))
    assert {path for path, headers in origin.snapshot()} == {
        '/image.png', '/animation.gif', '/audio.wav', '/video.mp4', '/page', '/denied', '/failed',
        '/automatic-page', '/automatic-thumbnail.png', '/automatic-noimage', '/automatic-broken',
        '/broken.png'}
    if os.environ.get('BLASTIRC_SMOKE_YOUTUBE') == '1':
        # ponytail: reuse decoded-only HTML card geometry and native Info;
        # remote JPEG colors and the current public title are not test fixtures.
        public_ca = Path('/etc/ssl/certs/ca-certificates.crt')
        assert os.access(public_ca, os.R_OK), 'YouTube opt-in requires readable platform public roots'
        combined_ca = Path(ctx['lab'].root) / 'media-public-and-fixture-ca.pem'
        with os.fdopen(os.open(combined_ca, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), 'wb') as bundle:
            bundle.write(public_ca.read_bytes() + b'\n' + Path(ctx['lab'].ca_file).read_bytes())
        ctx['env'] = dict(ctx['env'], SSL_CERT_FILE=str(combined_ca),
                          NIX_SSL_CERT_FILE=str(combined_ca), CURL_CA_BUNDLE=str(combined_ca))
        _refresh_restart(ctx, 'previews',
            dict(links='"automatic"', images='"automatic"', media='"click"', allow_local='false',
                 max_height='120', media_max_mib='16', media_max_seconds='60'),
            focus=ctx['query_id'])
        window = ctx['window']
        _, reference = html_rows('Native public YouTube plain reference', ctx['own_nick'])
        reference_gap = reference[2][1] - reference[0][1]
        url = 'https://www.youtube.com/watch?v=PtS2EdUsMeo'
        colors, _ = html_rows(url, ctx['own_nick'])
        def youtube_card():
            boxes = html_boxes(colors)
            gap = boxes[2][1] - boxes[0][1]
            return boxes if gap > reference_gap + 64 else None
        boxes = gui.SESSION.until('public YouTube decoded-only native thumbnail card', youtube_card)
        thumbnail_capture = str(gui.capture_window(window, Path(ctx['destination']),
                                                   'media-public-youtube-thumbnail'))
        point = (boxes[0][0] + 40, (boxes[0][3] + boxes[2][1]) / 2)
        popup = gui.open_native_popup(window, lambda: (
            gui.move_pointer(window, *point),
            gui.POINTER.button(True, code=273), gui.POINTER.button(False, code=273)))
        gui.popup_keys(window, popup, 'Home', 'Return')
        info = gui.SESSION.until('public YouTube original URL and native metadata Info',
            lambda: state if (state := gui.native_input_state(window))
            and state.get('text', '').splitlines()
            and state['text'].splitlines()[0] == url else None)
        parts = info['text'].split('\n\n')
        assert len(parts) >= 3 and parts[1].strip(), ('Missing actual YouTube title', info)
        assert 'Type: Image' in info['text'] and re.search(
            r'Dimensions: [1-9]\d* × [1-9]\d*', info['text']), info
        assert '\nThumbnail URL: https://' in info['text'], info
        evidence.append({'asset': 'public-youtube', 'internet_opt_in': True,
                         'postedURL': url, 'metadata_title': parts[1],
                         'native_info_text': info['text'],
                         'thumbnail_capture': thumbnail_capture, 'native_boxes': boxes,
                         'decoded_card_row_gap': boxes[2][1] - boxes[0][1],
                         'plain_reference_row_gap': reference_gap,
                         'links': 'automatic', 'images': 'automatic', 'media': 'click',
                         'allow_local': False})
        gui.framed_keys(window, 'Escape')
        upload.focus_composer(window)
        gui._composer_native(window, '', 0, 0)
    return {**_refresh_return(ctx, 'media', evidence),
            'private_fixture_only': os.environ.get('BLASTIRC_SMOKE_YOUTUBE') != '1'}


import subprocess
import re
import sqlite3
import tomllib


def run_badges(ctx):
    """Consume actual wire unread state and native raster; never mutate the model/DB."""
    p, w = ctx['proc'], ctx['window']
    prefs, db, args = ctx['preferences'], ctx['database_path'], ctx['args']
    palette_path = prefs.parent / 'noctalia.toml'
    # The harness owns this profile. Never consult the workstation's theme.
    assert args.palette is not None, 'badges requires --palette PATH to an owned generated test palette'
    assert palette_path.is_file(), ('Missing copied private palette', palette_path)
    colors = tomllib.loads(palette_path.read_text())['colors']

    def rgba(value):
        assert len(value) in (7, 9) and value.startswith('#'), value
        v = tuple(int(value[i:i + 2], 16) for i in range(1, len(value), 2))
        return v[:3], v[3] / 255 if len(v) == 4 else 1

    def luminance(rgb):
        channels = [v / 255 for v in rgb]
        channels = [v / 12.92 if v <= .04045 else ((v + .055) / 1.055) ** 2.4
                    for v in channels]
        return sum(a * b for a, b in zip(channels, (.2126, .7152, .0722)))

    def contrast(a, b):
        a, b = luminance(a), luminance(b)
        return (max(a, b) + .05) / (min(a, b) + .05)

    primary, _ = rgba(colors['primary'])
    error, _ = rgba(colors['error'])
    # This GUI fixture supplies readable authored pairs. Native theme consumer
    # checks own inaccessible-palette repair; do not duplicate its selection rule.
    on_primary, primary_alpha = rgba(colors['on_primary'])
    on_error, error_alpha = rgba(colors['on_error'])
    assert primary_alpha == error_alpha == 1, 'Use opaque authored badge foregrounds in this fixture'
    assert contrast(primary, on_primary) >= 4.5, ('Use a readable authored primary pair', primary, on_primary)
    assert contrast(error, on_error) >= 4.5, ('Use a readable authored error pair', error, on_error)
    report = {'palette': str(palette_path), 'plates': [],
              'unread_source': 'actual background PRIVMSG arrivals and Core Save DB barriers',
              'search_only_exclusion': 'CPU railReadAllCheck covers search-only unread; native state is wire-driven'}
    database = sqlite3.connect(f'file:{db}?mode=ro', uri=True)

    def saved():
        return gui.save_state(p, prefs, db)

    def layout():
        return gui.read_layout(prefs)

    def footer_point():
        x, y, width, height = layout()['rail']['rect']
        extent, pad, gap = max(32, 36 * args.zoom), 6 * args.zoom, 6 * args.zoom
        top = max(pad, height - pad - 3 * extent - 2 * gap)
        return (x + width / 2, y + top + extent / 2), extent, gap

    def nonsearch_unread():
        return database.execute("SELECT id,unread FROM buffers WHERE kind<>'search' AND unread>0 ORDER BY id").fetchall()

    def click_read():
        point, extent, gap = footer_point()
        assert nonsearch_unread(), 'Read action must be justified by durable nonsearch unread'
        marker = gui.GUI_DEBUG[w][1].stat().st_size
        gui.click(w, *point)
        gui.await_native_frame(w, marker, re.compile(r'wl_pointer[@#]\d+\.button\(\d+,\s*\d+,\s*272,\s*0\)'))
        saved()
        gui.wait_rows(p, db, "SELECT COUNT(*) FROM buffers WHERE kind<>'search' AND unread<>0", (), [(0,)])
        return {'read': point, 'add': (point[0], point[1] + extent + gap)}

    def rail_row(image):
        (cx, cy), extent, _ = footer_point()
        x, _, width, _ = layout()['rail']['rect']
        left, right = round(x + 6 * args.zoom), round(x + width - 6 * args.zoom)
        return b''.join(image['pixels'][(row * image['width'] + left) * 3:
                                      (row * image['width'] + right) * 3]
                        for row in range(round(cy - extent / 2), round(cy + extent / 2)))

    def capture():
        gui.move_pointer(w, 0, 0)
        return gui.native_rgb_capture(w)

    def plates(mode, role, foreground):
        gui.set_navigation(p, w, prefs, db, mode, 'refresh-badges-' + mode)
        bounds = layout()
        fill = primary if role == 'primary' else error

        def locate():
            image = capture()
            found = {}
            for pane in ('channels', 'rail'):
                x, y, width, height = bounds[pane]['rect']
                candidates = []
                for component in gui.native_marker_components(image, (x, y, width, height), fill):
                    left, top, right, bottom = component['bbox']
                    bw, bh = right - left, bottom - top
                    if 14 * args.zoom <= bh <= 24 * args.zoom and bh - 2 <= bw <= 60 * args.zoom:
                        # Row right excludes pane padding and the expanded
                        # scrollbar gutter (6px track + 8px gap), then badge pad.
                        inset = (4 + 2 if mode == 'compact' else 8 + 14 + 9) * args.zoom
                        if pane != 'channels' or abs(right - (x + width - inset)) <= 3:
                            candidates.append(component)
                if not candidates:
                    return False
                found[pane] = candidates
            return image, found

        image, found = gui.SESSION.until('opaque ' + role + ' unread plates in ' + mode, locate)

        def pixel(x, y):
            start = (y * image['width'] + x) * 3
            return tuple(image['pixels'][start:start + 3])

        for pane, components in found.items():
            for component in components:
                left, top, right, bottom = component['bbox']
                width, height = right - left, bottom - top
                radius = 4 * args.zoom
                # Flat full-color edges surrounding real glyph holes, not a bare count,
                # translucent tint, capsule, or fully square box.
                assert component['pixels'] >= width * height * .55, (mode, pane, component)
                mid_x, mid_y = (left + right - 1) // 2, (top + bottom - 1) // 2
                assert pixel(left, mid_y) == pixel(right - 1, mid_y) == fill
                assert pixel(mid_x, top) == pixel(mid_x, bottom - 1) == fill
                assert all(pixel(x, y) != fill for x, y in
                           ((left, top), (right - 1, top), (left, bottom - 1), (right - 1, bottom - 1))), component
                inset = next(x - left for x in range(left, right) if pixel(x, top) == fill)
                # A 4*scale radius raster has a short inset; a capsule has ~height/2.
                assert max(1, radius - 2) <= inset <= radius + 1, (mode, pane, 'radius', inset, radius)
                assert inset < height * .4, (mode, pane, 'capsule-shaped unread', inset, height)
                pad = max(1, round(2 * args.zoom))
                glyph = [pixel(x, y) for y in range(top + pad, bottom - pad)
                         for x in range(left + pad, right - pad)]
                # Small count glyphs need not contain a fully covered pixel.
                # Require meaningful ink on the actual fill->foreground blend
                # segment, rather than an incidental exact antialias endpoint.
                direction = tuple(ink - base for ink, base in zip(foreground, fill))
                length_squared = sum(value * value for value in direction)
                covered_ink = []
                for rgb in glyph:
                    coverage = sum((value - base) * delta for value, base, delta
                                   in zip(rgb, fill, direction)) / length_squared
                    residual = sum((value - (base + coverage * delta)) ** 2
                                   for value, base, delta in zip(rgb, fill, direction))
                    if .5 <= coverage <= 1.02 and residual <= 100:
                        covered_ink.append((coverage, rgb))
                assert len(covered_ink) >= 2, (
                    mode, pane, 'Missing published contrast foreground ink', foreground, component)
                assert contrast(fill, foreground) >= 4.5
                report['plates'].append({'mode': mode, 'pane': pane, 'role': role,
                                         'bbox': component['bbox'], 'opaque_pixels': component['pixels'],
                                         'foreground_pixels': len(covered_ink),
                                         'maximum_foreground_coverage': max(value[0] for value in covered_ink),
                                         'observed_ink_rgb': max(covered_ink)[1],
                                         'contrast': contrast(fill, foreground)})

    try:
        original = saved()
        original_mode = layout()['navigation_intent']
        opener = 'Native badge probe private query opener'
        ctx['observer'].send(f"PRIVMSG {ctx['own_nick']} :{opener}")
        opener_rows = [(ctx['query_id'], ctx['query_target'], opener, 0, 0)]
        ctx['expected_messages'].extend(opener_rows)
        gui.wait_messages(p, db, opener_rows)
        gui.select_native_buffer(p, w, prefs, db, args, ctx['query_id'])
        if nonsearch_unread():
            click_read()
        gui.set_navigation(p, w, prefs, db, 'expanded', 'refresh-badges-baseline')
        assert not nonsearch_unread()
        empty_row = rail_row(capture())
        upload.focus_composer(w)
        gui.wait_read_marker(p, db, ctx['query_id'])
        before_input = gui.native_input_state(w)
        assert before_input is not None
        draft = before_input['text'] + ' private unrelated badge draft'
        gui.type_text(' private unrelated badge draft')
        saved()
        gui.wait_rows(p, db, 'SELECT draft FROM buffers WHERE id=?', (ctx['query_id'],), [(draft,)])
        protected = database.execute('SELECT id,draft FROM buffers ORDER BY id').fetchall()
        query_marker = database.execute('SELECT read_marker FROM buffers WHERE id=?', (ctx['query_id'],)).fetchone()

        for mention, role, foreground in ((False, 'primary', on_primary), (True, 'error', on_error)):
            text = ('Native unread plate ordinary arrival' if not mention else
                    ctx['own_nick'] + ': Native unread plate mention arrival')
            ctx['observer'].send(f"PRIVMSG {ctx['channel']} :{text}")
            added = ctx['channel_messages'](text, ctx['query_target'])
            ctx['expected_messages'].extend(added)
            gui.wait_messages(p, db, added)
            saved()  # Core Save is the unread snapshot barrier, not a passive read.
            for identity in (ctx['channel_id'], ctx['other_channel_id']):
                gui.SESSION.until('durable real-wire unread ' + identity,
                    lambda identity=identity: database.execute(
                        'SELECT unread FROM buffers WHERE id=?', (identity,)).fetchone()[0] > 0)
            for mode in ('expanded', 'compact'):
                plates(mode, role, foreground)

        gui.set_navigation(p, w, prefs, db, 'expanded', 'refresh-badges-read-action')
        history = database.execute('SELECT * FROM messages WHERE presence=0 ORDER BY seq').fetchall()
        before = nonsearch_unread()
        visible_row = rail_row(capture())
        assert visible_row != empty_row, 'Conditional read row never appeared above Add'
        # Prove this retained action by its native click and durable effects,
        # not by incidental tooltip text or tooltip presentation timing.
        report['global_read'] = click_read()
        gui.move_pointer(w, 0, 0)
        gui.SESSION.until('conditional read row retired above unchanged Add/Settings',
                          lambda: rail_row(capture()) == empty_row)
        assert database.execute('SELECT * FROM messages WHERE presence=0 ORDER BY seq').fetchall() == history
        assert database.execute('SELECT id,draft FROM buffers ORDER BY id').fetchall() == protected
        assert database.execute('SELECT read_marker FROM buffers WHERE id=?', (ctx['query_id'],)).fetchone() == query_marker
        for identity, _ in before:
            gui.wait_read_marker(p, db, identity)
        assert report['global_read']['read'][1] < report['global_read']['add'][1]
        report['global_read'].update(unread_before=before, history_preserved=True,
                                     drafts_preserved=True, retired_when_nonsearch_read=True)
        gui.select_native_buffer(p, w, prefs, db, args, ctx['query_id'])
        upload.focus_composer(w)
        current = gui.native_input_state(w)
        assert current and current['text'] == draft, ('Read action lost unrelated native draft', current)
        # Remove only this probe's suffix through native editing; retain prior draft.
        gui.framed_keys(w, *(['shift+Left'] * len(' private unrelated badge draft')))
        gui.framed_keys(w, 'BackSpace')
        saved()
        gui.wait_rows(p, db, 'SELECT draft FROM buffers WHERE id=?', (ctx['query_id'],), [(before_input['text'],)])
        gui.set_navigation(p, w, prefs, db, original_mode, 'refresh-badges-restore')
        gui.select_native_buffer(p, w, prefs, db, args, original[1][0])
        return report
    finally:
        database.close()


# Integration section: import gui_smoke as gui in the receiving script.
# Only private HeadlessSway/LocalTopology windows and their clipboard are used.
import csv
import io
import re
import sqlite3
import subprocess
import time
import tomllib
from pathlib import Path


def _policy_settings(ctx, category, edits):
    """Drive real controls; edits are (tab stop, kind, value, section, key)."""
    w, prefs = ctx['window'], Path(ctx['preferences'])
    gui.open_settings(w, prefs)
    gui.framed_keys(w, 'shift+Tab')
    gui.settings_category(w, ctx['args'].zoom, category)
    for stop, kind, value, section, name in edits:
        gui.framed_keys(w, *(['Tab'] * stop))
        if kind == 'select':
            gui.settings_select(w, value)
        elif kind == 'slider':
            # Home is the native slider minimum; Right is one declared step.
            minimum, step, target = value
            gui.framed_keys(w, 'Home', *(['Right'] * round((target - minimum) / step)))
        elif kind == 'text':
            gui.settings_field(w, value, ctx['destination'], 'policy-' + name)
        else:
            assert kind == 'toggle'
            current = tomllib.loads(prefs.read_text())[section][name]
            if current != value:
                gui.framed_keys(w, 'Return')
        gui.framed_keys(w, *(['shift+Tab'] * stop))
    saved = gui.save_settings_footer(w, saved_path=prefs)
    for _, kind, value, section, name in edits:
        actual = saved[section][name] if section else saved[name]
        if not section and name == 'fools' and isinstance(actual, dict):
            actual = actual['names']
        if kind == 'slider':
            assert abs(actual - value[2]) < .001, (name, actual, value)
        elif kind != 'select':
            expected = value.split() if not section and name == 'fools' else value
            assert actual == expected, (name, actual, expected)
    gui.close_settings_navigation(w)
    gui.wait_layout(ctx['proc'], prefs, predicate=lambda layout: 'settings' not in layout)
    return saved


def _policy_appearance(ctx, *, fool=None, unread=None, wrap=None):
    edits = []
    if wrap is not None:
        edits.append((21, 'slider', (0, 1, wrap), 'appearance', 'message_wrap_columns'))
    if fool is not None:
        edits.append((22, 'select', ('minimized', 'blurred', 'dimmed', 'hidden').index(fool),
                      'appearance', 'fool_display'))
    if unread is not None:
        edits.append((23, 'toggle', unread, 'appearance', 'fool_unread'))
    saved = _policy_settings(ctx, 0, edits)
    if fool is not None:
        assert saved['appearance']['fool_display'] == fool, saved['appearance']
    return saved


def _policy_chat(ctx, *, fools=None, receive=None):
    saved = tomllib.loads(Path(ctx['preferences']).read_text())
    provider = saved['uploads']['provider']
    upload_stops = 2 + (2 if provider in ('crafterbin', 'custom') else 0)
    upload_stops += (1 if provider == 'litterbox' else 0) + (3 if provider == 'custom' else 0)
    edits = []
    if receive is not None:
        edits.append((upload_stops + 3, 'toggle', receive, 'typing', 'receive'))
    if fools is not None:
        edits.append((upload_stops + 5, 'text', ' '.join(fools), '', 'fools'))
    return _policy_settings(ctx, 2, edits)


def _policy_effects(ctx, *, motion='none', reveal='none', fx='full', reduced=False):
    saved = _policy_settings(ctx, 3, [
        (1, 'select', ('off', 'subtle', 'full').index(fx), 'appearance', 'fx'),
        (2, 'toggle', reduced, 'appearance', 'reduced_motion'),
        (5, 'select', ('none', 'rise', 'pop', 'slide', 'drift').index(motion),
         'appearance', 'incoming_motion'),
        (6, 'select', ('none', 'fade', 'pixels').index(reveal), 'appearance', 'incoming_reveal'),
        (7, 'slider', (100, 10, 2000), 'appearance', 'incoming_duration_ms')])
    appearance = saved['appearance']
    assert all(appearance[k] == v for k, v in {
        'fx': fx, 'reduced_motion': reduced, 'incoming_motion': motion,
        'incoming_reveal': reveal, 'incoming_duration_ms': 2000}.items()), appearance


def _policy_focus(ctx, identity, *, expected=('', 0, 0)):
    if identity == ctx['query_id']:
        with sqlite3.connect(f'file:{ctx["database_path"]}?mode=ro', uri=True) as database:
            exists = database.execute('SELECT 1 FROM buffers WHERE id=?', (identity,)).fetchone()
        if not exists:
            _policy_send(ctx, 'Native policy query opener')
    gui.select_native_buffer(ctx['proc'], ctx['window'], ctx['preferences'],
                             ctx['database_path'], ctx['args'], identity)
    gui._composer_native(ctx['window'], *expected)


def _policy_send(ctx, text, *, target=None, durable=True):
    target = target or ctx['own_nick']
    line = f'PRIVMSG {target} :{text}'
    assert len(line.encode('utf-8')) <= 510, 'Fixture exceeds IRC line limit'
    ctx['observer'].send(line)
    rows = ([(ctx['query_id'], ctx['query_target'], text, 0, 0)]
            if target == ctx['own_nick'] else ctx['channel_messages'](text, ctx['query_target']))
    ctx['expected_messages'].extend(rows)
    if durable:
        gui.wait_messages(ctx['proc'], ctx['database_path'], ctx['expected_messages'])
    return line


def _policy_capture(ctx, label):
    return gui.capture_window(ctx['window'], Path(ctx['destination']), 'policy-' + label)


def _policy_words(ctx, label, rectangle=None):
    image = _policy_capture(ctx, label)
    result = subprocess.run(['tesseract', str(image), 'stdout', '--psm', '11', 'tsv'],
                            check=True, capture_output=True, text=True, timeout=20)
    words = []
    for row in csv.DictReader(io.StringIO(result.stdout), delimiter='\t'):
        if not row.get('text', '').strip():
            continue
        x, y, width, height = (int(row[key]) for key in ('left', 'top', 'width', 'height'))
        if rectangle is not None:
            rx, ry, rw, rh = rectangle
            if not (rx <= x < rx + rw and ry <= y < ry + rh):
                continue
        words.append((row['text'], (x, y, x + width, y + height)))
    return words


def _policy_reply_region(ctx, label):
    return gui.native_reply_excerpt_region(ctx['window'], ctx['preferences'],
                                            Path(ctx['destination']), label)


def _policy_chip_words(ctx, image, region, reference_height, label, allow_absent=False):
    """Recognize observed chip ink, returning native boxes and capture evidence."""
    native = (image if isinstance(image, Image.Image) else
              Image.frombytes('RGB', (image['width'], image['height']), image['pixels']))
    body = native.crop(region)
    width, height = body.size
    assert width > 0 and height > 0, ('Invalid chip region', region)
    pixels = body.load()
    # Flat chip fill is connected around its glyph holes. Body background
    # reaches the crop edge and cannot satisfy the isolated-chip geometry.
    colors = body.getcolors(width * height)
    candidates = []
    for count, fill in colors:
        if count < max(24, reference_height * 2):
            continue
        remaining = {(x, y) for y in range(height) for x in range(width)
                     if pixels[x, y] == fill}
        while remaining:
            seed = remaining.pop()
            component, pending = {seed}, [seed]
            while pending:
                x, y = pending.pop()
                for point in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
                    if point in remaining:
                        remaining.remove(point)
                        component.add(point)
                        pending.append(point)
            if len(component) < max(24, reference_height * 2):
                continue
            left = min(x for x, y in component)
            top = min(y for x, y in component)
            right = max(x for x, y in component) + 1
            bottom = max(y for x, y in component) + 1
            w, h = right - left, bottom - top
            if not (0 < left < right < width and 0 < top < bottom < height
                    and w > h and .7 * reference_height <= h <= reference_height + 8
                    and len(component) >= .65 * w * h):
                continue
            # Common inner row span excludes dark rounded-corner pixels.
            rows = [[x for x in range(left, right) if (x, y) in component]
                    for y in range(top + 1, bottom - 1)]
            if not rows or any(not row for row in rows):
                continue
            inner_left = max(min(row) for row in rows) + 1
            inner_right = min(max(row) for row in rows)
            if inner_right - inner_left < w / 2:
                continue
            candidates.append((fill, (left, top, right, bottom),
                               (inner_left, top + 1, inner_right, bottom - 1)))
    if not candidates and allow_absent:
        return [], None
    assert len(candidates) == 1, ('Expected one isolated flat chip', candidates, region)
    fill, fill_box, interior = candidates[0]
    glyphs = body.crop(interior).convert('L')
    fill_luma = Image.new('RGB', (1, 1), fill).convert('L').getpixel((0, 0))
    darkest = glyphs.getextrema()[0]
    assert fill_luma - darkest >= 48, ('Chip lacks contrasting dark glyph ink', fill, darkest)
    # Normalize observed fill->ink contrast without binarizing native glyphs:
    # binary OCR input can discard antialiased letter-closure detail.
    ink = glyphs.point(lambda value: max(0, min(255, round(
        (fill_luma - value) * 255 / (fill_luma - darkest)))))
    ink_box = ink.point(lambda value: 255 if value >= 32 else 0).getbbox()
    assert ink_box is not None, ('Chip has no contrast glyphs', fill_box)
    scale, border = 4, 16
    tight = ink.crop(ink_box).point(lambda value: 255 - value)
    enlarged = tight.resize((tight.width * scale, tight.height * scale), Image.Resampling.LANCZOS)
    ocr_image = Image.new('L', (enlarged.width + 2 * border, enlarged.height + 2 * border), 255)
    ocr_image.paste(enlarged, (border, border))
    mask_path = Path(str(ctx['destination']).removesuffix('.bmp') + '-' + label + '-mask.png')
    ocr_image.save(mask_path)
    result = subprocess.run(['tesseract', str(mask_path), 'stdout', '--psm', '7',
                             '-c', 'load_system_dawg=0', '-c', 'load_freq_dawg=0', 'tsv'],
                            check=True, capture_output=True, text=True, timeout=20)
    origin_x = region[0] + interior[0] + ink_box[0]
    origin_y = region[1] + interior[1] + ink_box[1]
    words = []
    for row in csv.DictReader(io.StringIO(result.stdout), delimiter='\t'):
        if not row.get('text', '').strip():
            continue
        x, y, w, h = (int(row[key]) for key in ('left', 'top', 'width', 'height'))
        box = (origin_x + (x - border) // scale,
               origin_y + (y - border) // scale,
               origin_x + (x + w - border + scale - 1) // scale,
               origin_y + (y + h - border + scale - 1) // scale)
        words.append((row['text'].strip(), box))
    evidence = {'fill_rgb': fill,
                'fill_box': (region[0] + fill_box[0], region[1] + fill_box[1],
                             region[0] + fill_box[2], region[1] + fill_box[3]),
                'native_ink_box': (origin_x, origin_y,
                                   origin_x + tight.width, origin_y + tight.height),
                'mask_path': str(mask_path), 'words': words}
    return words, evidence


def _policy_box(ctx, rgb, *, image=None):
    image = image or gui.native_rgb_capture(ctx['window'])
    rectangle = gui.read_layout(ctx['preferences'])['conversation']['rect']
    items = gui.native_marker_components(image, rectangle, rgb)
    return items[0]['bbox'] if len(items) == 1 and items[0]['full'] else None


def _policy_wait_box(ctx, rgb):
    return gui.SESSION.until('actual settled policy body marker', lambda: _policy_box(ctx, rgb))


def _policy_crop(image, box):
    left, top, right, bottom = map(round, box)
    assert 0 <= left < right <= image['width'] and 0 <= top < bottom <= image['height'], box
    stride = image['width'] * 3
    return b''.join(image['pixels'][y * stride + left * 3:y * stride + right * 3]
                    for y in range(top, bottom))


def _policy_drag_copy(ctx, start, end, expected=None, forbidden=None):
    w = ctx['window']
    before = gui.native_input_state(w)
    sentinel = 'private-policy-copy-sentinel'
    gui.seed_clipboard(sentinel, ctx['cleanup'])
    gui.move_pointer(w, *start)
    marker = gui.GUI_DEBUG[w][1].stat().st_size
    gui.pointer_button(True)
    gui.move_pointer(w, *end)
    gui.pointer_button(False)
    gui.await_native_frame(w, marker,
        re.compile(r'wl_pointer[@#]\d+\.button\(\d+,\s*\d+,\s*272,\s*0\)'))
    # Copy might intentionally do nothing for an unreadable row; key release is
    # the finite barrier, not an expectation that suppressed copy paints.
    marker = gui.GUI_DEBUG[w][1].stat().st_size
    code = gui.key('ctrl+c', trace=True)[-1]
    released = re.compile(rf'wl_keyboard[@#]\d+\.key\(\d+,\s*\d+,\s*{code},\s*0\)')
    gui.SESSION.until('actual private copy key release', lambda: released.search(
        gui.GUI_DEBUG[w][1].read_bytes()[marker:].decode(errors='replace')))
    if expected is not None:
        gui.SESSION.until('exact original decoded selection clipboard', lambda: gui.clipboard_text() == expected)
    copied = gui.clipboard_text()
    if forbidden is not None:
        assert forbidden not in (copied or ''), ('Unreadable body leaked to selection', copied)
    assert gui.native_input_state(w) == before, 'Transcript selection mutated native composer'
    return copied


def _policy_typing_cap(ctx):
    observer = ctx['observer']
    observer.send('CAP REQ :message-tags echo-message')
    observer.wait(lambda command, params, prefix: command == 'CAP' and
                  ('ACK' in params or 'NAK' in params), timeout=15)
    assert {'message-tags', 'echo-message'} <= observer.capabilities, observer.capabilities
    assert gui.client_tags_allowed(observer, '+typing'), observer.isupport


def _policy_typing(ctx, state):
    observer = ctx['observer']
    observer.send(f'@+typing={state} TAGMSG {ctx["own_nick"]}')
    observer.wait(lambda command, params, prefix: command == 'TAGMSG' and
                  params == [ctx['own_nick']] and prefix.split('!', 1)[0] == ctx['query_target'] and
                  observer.last_tags.get('+typing') == state, timeout=15)


def _policy_search_privacy(ctx, secret, mode, *, evidence=None):
    # Search the actual retained café body; do not put the secret itself in the
    # query/title, where its appearance would be unrelated to result privacy.
    w = ctx['window']
    gui.framed_keys(w, 'ctrl+s')
    gui.type_text('café')
    gui.framed_keys(w, 'Return')
    gui.SESSION.until('native search results hide composer', lambda: gui.native_input_state(w) is None)
    # Settings Save persists both the focused pane and Core selection; its
    # temporary editor focus is not evidence of which result buffer is active.
    workspace, active, _ = gui.save_state(ctx['proc'], ctx['preferences'], ctx['database_path'])
    selected = workspace['visible_buffers']
    assert active == (selected,), ('Search pane and Core selection disagree', workspace, active)
    with sqlite3.connect(f'file:{ctx["database_path"]}?mode=ro', uri=True) as database:
        metadata = database.execute(
            'SELECT result.kind,result.target,result.server=source.server FROM buffers AS result '
            'JOIN buffers AS source ON source.id=? WHERE result.id=?',
            (ctx['query_id'], selected)).fetchone()
    assert metadata == ('search', '*search*', 1), ('Search did not select native results', selected, metadata)
    layout = gui.read_layout(ctx['preferences'])
    x, y, width, height = layout['conversation']['rect']
    metrics = gui.pane_metrics(layout, layout['scale'])
    # Search panes have no composer (PaneView::sync/doLayout). Scope OCR to
    # their transcript below the title, never the query dialog or sidebar.
    rectangle = (x + metrics['left'], y + metrics['top'],
                 metrics['inner_width'], height - metrics['top'])
    marker = gui.GUI_DEBUG[w][1].stat().st_size
    gui.click(w, x + width / 2, y + (metrics['top'] + height) / 2)
    gui.await_native_frame(w, marker,
        re.compile(r'wl_pointer[@#]\d+\.button\(\d+,\s*\d+,\s*272,\s*1\)'))
    gui.SESSION.until('restored native search transcript focus',
                      lambda: gui.native_input_state(w) is None)
    words = _policy_words(ctx, 'search-' + mode, rectangle)
    rendered = ' '.join(word for word, _ in words)
    if mode != 'dimmed':
        assert secret.casefold() not in rendered.casefold(), ('Fool body leaked through search', mode, words)
    else:
        # Locate body ink, not the secret's font-dependent OCR spelling.
        cafes = [box for word, box in words if word.casefold() == 'café']
        assert len(cafes) == 2, ('Readable dim search bodies missing', words)
        box = max(cafes, key=lambda item: item[1])
        point = ((box[0] + box[2]) / 2, (box[1] + box[3]) / 2)
        image = gui.native_rgb_capture(w)
        row_y = int(point[1])
        start = (row_y * image['width'] + int(rectangle[0])) * 3
        end = (row_y * image['width'] + int(rectangle[0] + rectangle[2])) * 3
        row = image['pixels'][start:end]
        colors = {tuple(row[index:index + 3]) for index in range(0, len(row), 3)}
        markers = []
        for side in ('left', 'right'):
            candidates = []
            for color in colors:
                red, green, blue = color
                # Discover alpha-composited fixture swatches without assuming
                # opacity or a theme background; require real solid rectangles.
                if not (green > max(red, blue) if side == 'left' else min(red, blue) > green):
                    continue
                for component in gui.native_marker_components(image, rectangle, color):
                    left, top, right, bottom = component['bbox']
                    if (component['full'] and top <= row_y < bottom and
                            (right < box[0] if side == 'left' else left > box[2])):
                        candidates.append({'rgb': color, 'box': component['bbox']})
            assert len(candidates) == 1, ('Dim search body marker missing', side, point, candidates)
            markers.append(candidates[0])
        # Local search preserves chronological fixture order and wraps the
        # original text with [target] nick:. Copy text decodes IRC formatting.
        raw = ('\x042AB971,2AB971LEFTLEFT\x0f ' + secret +
               ' café e\u0301 中 \x04B34FD5,B34FD5RGHTRGHT\x0f second')
        with sqlite3.connect(f'file:{ctx["database_path"]}?mode=ro', uri=True) as database:
            retained = database.execute(
                'SELECT nick,text FROM messages WHERE buffer=? AND text=?',
                (ctx['query_id'], raw)).fetchall()
        assert retained == [(ctx['query_target'], raw)], ('Original search fixture identity lost', retained)
        expected = ('[' + ctx['query_target'] + '] ' + retained[0][0] +
                    ': LEFTLEFT ' + secret + ' café e\u0301 中 RGHTRGHT second')
        sentinel = 'private-policy-search-copy-sentinel'
        gui.seed_clipboard(sentinel, ctx['cleanup'])
        popup = gui.open_native_popup(w, lambda: (
            gui.move_pointer(w, *point),
            gui.POINTER.button(True, code=273), gui.POINTER.button(False, code=273)))
        gui.popup_keys(w, popup, 'Home', 'Return', root_changed=False)
        capture = _policy_capture(ctx, 'search-dimmed-copy')
        gui.SESSION.until('native search Copy text replaces private sentinel',
                          lambda: gui.clipboard_text() not in (None, sentinel))
        copied = gui.clipboard_text()
        copy_evidence = {'point': point, 'body_box': box, 'markers': markers,
                         'expected_text': expected, 'copied_text': copied,
                         'retained_raw': retained[0][1], 'capture': str(capture)}
        if evidence is not None:
            evidence['search_copy'] = copy_evidence
        assert copied == expected, ('Dim search Copy text changed original identity', copy_evidence)
    gui.switch_buffer(ctx['query_target'], 'Local')
    gui._composer_native(w, '', 0, 0)
    return words


def run_activity_blur(ctx):
    """Real collapsed mixed-sender activity stays sharp beside a concealed fool."""
    import contextlib
    import os

    p, w, prefs, db = ctx['proc'], ctx['window'], ctx['preferences'], ctx['database_path']
    channel, identity = ctx['channel'], ctx['channel_id']
    _policy_focus(ctx, identity)
    _policy_effects(ctx, fx='off')
    _policy_chat(ctx, fools=[])
    _policy_appearance(ctx, fool='dimmed')
    gui.set_message_style(p, w, prefs, db, ctx['args'], ctx['destination'],
                          identity, 'irc', label='activity-blur-irc')
    root = gui.open_title_menu(w, gui.title_actions(gui.read_layout(prefs), ctx['args'].zoom))
    gui.open_native_submenu(w, root, 3, from_end=True)
    gui.popup_keys(w, root, 'Home', 'Down', 'Down', 'Return')
    gui.wait_preference(p, prefs, 'presence', 'channels',
        dict(tomllib.loads(Path(prefs).read_text())['presence']['channels'], **{identity: 'smart'}))
    nonce = os.urandom(4).hex()
    fool = 'fb' + nonce
    colors = ((45, 155, 217), (217, 69, 119), (42, 185, 113))
    bodies = ['\x042D9BD9,2D9BD9BEFORE\x0f', '\x04D94577,D94577AFTER\x0f',
              '\x042AB971,2AB971CONCEALED\x0f private body']
    with contextlib.ExitStack() as stack:
        def guest(nick):
            return stack.enter_context(gui.fixtures.BootstrapClient(
                dict(ctx['lab'].endpoints['direct'], nick=nick, username=nick),
                ctx['lab'].ca_file, authenticate=False))

        concealed = guest(fool)
        concealed.send('JOIN ' + channel)
        gui.receive_until(ctx['observer'], 'JOIN', [channel], fool, redact=ctx['lab']._redact)
        nicks = ['ba' + nonce, 'bb' + nonce]
        participants = [(nick, guest(nick)) for nick in nicks]
        for nick, client in participants:
            client.send('JOIN ' + channel)
            gui.receive_until(ctx['observer'], 'JOIN', [channel], nick, redact=ctx['lab']._redact)
            contribution = 'synthetic participant ' + nick
            client.send(f'PRIVMSG {channel} :{contribution}')
            gui.receive_until(ctx['observer'], 'PRIVMSG', [channel, contribution], nick,
                              redact=ctx['lab']._redact)
            ctx['expected_messages'].extend(ctx['channel_messages'](contribution, nick))
        gui.wait_messages(p, db, ctx['expected_messages'])
        _policy_send(ctx, bodies[0], target=channel)
        with sqlite3.connect(f'file:{db}?mode=ro', uri=True) as database:
            frontier = database.execute('SELECT COALESCE(MAX(seq),0) FROM messages').fetchone()[0]
        for nick, client in participants:
            for line, command, params in (
                    ('PART ' + channel + ' :synthetic departure', 'PART', [channel, 'synthetic departure']),
                    ('JOIN ' + channel, 'JOIN', [channel]),
                    ('PART ' + channel + ' :synthetic departure', 'PART', [channel, 'synthetic departure'])):
                client.send(line)
                gui.receive_until(ctx['observer'], command, params, nick, redact=ctx['lab']._redact)
        typed = 'SELECT nick,activity FROM messages WHERE buffer=? AND seq>? AND presence=1 ORDER BY seq'
        details = [(nick, activity) for nick in nicks for activity in (2, 1, 2)]
        gui.wait_rows(p, db, typed, (identity, frontier), details)
        _policy_send(ctx, bodies[1], target=channel)
        concealed.send(f'PRIVMSG {channel} :{bodies[2]}')
        gui.receive_until(ctx['observer'], 'PRIVMSG', [channel, bodies[2]], fool,
                          redact=ctx['lab']._redact)
        ctx['expected_messages'].extend(ctx['channel_messages'](bodies[2], fool))
        gui.wait_messages(p, db, ctx['expected_messages'])
        upload.focus_composer(w)

        def anchors():
            image = gui.native_rgb_capture(w)
            rectangle = gui.read_layout(prefs)['conversation']['rect']
            items = [gui.native_marker_components(image, rectangle, color) for color in colors[:2]]
            if any(len(found) != 1 or not found[0]['full'] for found in items):
                return False
            return [found[0]['bbox'] for found in items]

        before, after = gui.SESSION.until('complete visible activity flanks', anchors)
        reference = gui.native_rgb_capture(w)
        fool_items = gui.native_marker_components(reference,
            gui.read_layout(prefs)['conversation']['rect'], colors[2])
        assert len(fool_items) == 1 and fool_items[0]['full'], fool_items
        fool_box = fool_items[0]['bbox']
        x, _, width, _ = gui.read_layout(prefs)['conversation']['rect']
        summary_box = (round(x + 4), round(before[3] + 2), round(x + width - 4), round(after[1] - 2))
        assert summary_box[3] > summary_box[1] + 8, (before, after, summary_box)
        original = _policy_crop(reference, summary_box)
        # A genuinely painted summary, not an empty/offscreen gap, must contain ink.
        assert len(set(tuple(original[i:i + 3]) for i in range(0, len(original), 3))) > 12
        reference_path = _policy_capture(ctx, 'ordinary-activity-reference')
        _policy_chat(ctx, fools=[fool])
        _policy_appearance(ctx, fool='blurred')
        current_flanks = gui.SESSION.until('visible flanks after concealment policy', anchors)
        dx, dy = current_flanks[0][0] - before[0], current_flanks[0][1] - before[1]
        translated = [tuple(value + (dx if index % 2 == 0 else dy)
                            for index, value in enumerate(box)) for box in (before, after)]
        assert current_flanks == translated, ('Activity relative geometry changed', translated, current_flanks)
        current = gui.native_rgb_capture(w)
        current_summary_box = (summary_box[0], summary_box[1] + dy,
                               summary_box[2], summary_box[3] + dy)
        actual = _policy_crop(current, current_summary_box)
        result_path = _policy_capture(ctx, 'ordinary-activity-blurred-policy')
        assert actual == original, (
            'Ordinary collapsed mixed-sender activity changed under fool blur policy',
            summary_box, str(reference_path), str(result_path))
        current_fool_box = tuple(value + (dx if index % 2 == 0 else dy)
                                 for index, value in enumerate(fool_box))
        assert _policy_crop(current, current_fool_box) != _policy_crop(reference, fool_box), \
            'Fool policy did not alter the actual visible reference body'
        # A vanished/offscreen fool is not evidence of blur. Its unique green
        # swatch must still contribute actual tinted pixels below the normal
        # flank and above the real composer, inside this conversation.
        cx, cy, cw, ch = gui.read_layout(prefs)['conversation']['rect']
        caret_top = gui.native_input_rectangle(w)[1]
        fool_region = (round(cx), round(current_flanks[1][3]),
                       round(cx + cw), round(min(cy + ch, caret_top)))
        tinted = _policy_crop(current, fool_region)
        visible_tint = sum(max(abs(tinted[i + c] - colors[2][c]) for c in range(3)) <= 32
                           for i in range(0, len(tinted), 3))
        assert visible_tint > 0, 'No positive rendered fool swatch remains inside the visible transcript'
        # Gaussian blur preserves the interior of a solid color block. Measure
        # actual adjacent glyph edges instead of requiring the green core to vanish.
        candidate = (fool_box[2] + 4, fool_box[1] - 2,
                     round(cx + cw - 8), fool_box[3] + 2)
        candidate_size = (candidate[2] - candidate[0], candidate[3] - candidate[1])
        reference_text = Image.frombytes('RGB', candidate_size, _policy_crop(reference, candidate))
        background = reference_text.getpixel((candidate_size[0] - 1, candidate_size[1] // 2))
        ink = ImageChops.difference(reference_text,
            Image.new('RGB', candidate_size, background)).getbbox()
        assert ink, 'Visible reference fool body has no glyph ink'
        text_box = (candidate[0] + ink[0], candidate[1] + ink[1],
                    candidate[0] + ink[2], candidate[1] + ink[3])
        blurred_text_box = tuple(value + (dx if index % 2 == 0 else dy)
                                 for index, value in enumerate(text_box))
        assert (fool_region[0] <= blurred_text_box[0] < blurred_text_box[2] <= fool_region[2]
                and fool_region[1] <= blurred_text_box[1] < blurred_text_box[3] <= fool_region[3]), \
            'Observed fool glyph region is outside the visible transcript'
        stride = (text_box[2] - text_box[0]) * 3
        def edge_energy(pixels):
            return sum(abs(pixels[index] - pixels[index + 3])
                       for index in range(len(pixels) - 3)
                       if index % stride < stride - 3)
        sharp_energy = edge_energy(_policy_crop(reference, text_box))
        blurred_energy = edge_energy(_policy_crop(current, blurred_text_box))
        assert 0 < blurred_energy < sharp_energy, (
            'Genuine fool glyphs are not visibly softened', sharp_energy, blurred_energy,
            text_box, blurred_text_box)
        middle_y = (blurred_text_box[1] + blurred_text_box[3]) / 2
        copied = _policy_drag_copy(ctx, (blurred_text_box[0] - 1, middle_y),
                                  (blurred_text_box[2] + 1, middle_y), forbidden='private body')
        assert copied == 'private-policy-copy-sentinel', 'Concealed fool changed the selection clipboard'
        # Expand through the genuine summary hit target; geometry proves this was
        # collapsed activity, not plain text that happened to resemble a summary.
        gui.click(w, x + width / 2, (current_summary_box[1] + current_summary_box[3]) / 2)
        expanded = gui.SESSION.until('actual activity expansion increases flank distance',
            lambda: boxes if (boxes := anchors()) and
            boxes[1][1] - boxes[0][1] > after[1] - before[1] + 6 else None)
        gui.wait_rows(p, db, typed, (identity, frontier), details)
        return _refresh_return(ctx, 'activity-blur', {
            'typed_activity': details, 'collapsed_flanks': [before, after],
            'expanded_flanks': expanded, 'summary_box': summary_box,
            'blurred_summary_box': current_summary_box, 'whole_block_translation': [dx, dy],
            'visible_blurred_fool_tint_pixels': visible_tint, 'fool_visible_region': fool_region,
            'fool_text_reference_box': text_box, 'fool_blurred_text_box': blurred_text_box,
            'fool_sharp_edge_energy': sharp_energy, 'fool_blurred_edge_energy': blurred_energy,
            'fool_unrevealed_clipboard': copied,
            'ordinary_summary_pixels_unchanged': True, 'adjacent_fool_concealed': True,
            'captures': [str(reference_path), str(result_path)]})


def run_fools(ctx):
    """All four real treatments, reveal/copy privacy, retained data and unread policy."""
    defaults = tomllib.loads(Path(ctx['preferences']).read_text())['appearance']
    assert defaults['fool_display'] == 'minimized' and defaults['fool_unread'] is False, defaults
    _policy_focus(ctx, ctx['query_id'])
    _policy_effects(ctx, fx='off')
    _policy_typing_cap(ctx)
    rgb = (42, 185, 113)
    secret = 'PrivateFoolBody'
    raw = '\x042AB971,2AB971LEFTLEFT\x0f ' + secret + ' café e\u0301 中 \x04B34FD5,B34FD5RGHTRGHT\x0f'
    _policy_send(ctx, raw)
    _policy_send(ctx, raw + ' second')
    _policy_chat(ctx, fools=[ctx['query_target'].swapcase()], receive=True)
    modes = {}
    for mode in ('minimized', 'blurred', 'dimmed', 'hidden'):
        # Readable reference gives real native hit geometry, not guessed font metrics.
        _policy_appearance(ctx, fool='dimmed', unread=False)
        # Dim opacity deliberately changes swatch RGB, so obtain the reference
        # without a fool list, then restore the same list before applying policy.
        _policy_chat(ctx, fools=[])
        # Two matching swatches exist; use their lowest actual complete component.
        components = gui.native_marker_components(gui.native_rgb_capture(ctx['window']),
            gui.read_layout(ctx['preferences'])['conversation']['rect'], rgb)
        assert len(components) == 2 and all(item['full'] for item in components), components
        left = max((item['bbox'] for item in components), key=lambda box: box[1])
        reference = gui.native_rgb_capture(ctx['window'])
        text_box = (left[2] + 6, left[1], left[2] + 140, left[3])
        _policy_chat(ctx, fools=[ctx['query_target'].swapcase()])
        _policy_appearance(ctx, fool=mode, unread=False)
        words = _policy_words(ctx, 'fools-' + mode)
        rendered = ' '.join(word for word, _ in words)
        modes[mode] = {'rendered_words': words, 'reference_body_marker': left}
        if mode == 'blurred':
            blurred = _policy_crop(gui.native_rgb_capture(ctx['window']), text_box)
            original = _policy_crop(reference, text_box)
            def edge_energy(pixels):
                stride = (text_box[2] - text_box[0]) * 3
                return sum(abs(pixels[index] - pixels[index + 3])
                           for index in range(len(pixels) - 3)
                           if index % stride < stride - 3)
            assert blurred != original and 0 < edge_energy(blurred) < edge_energy(original), (
                'Blurred treatment is not a real softened rendering of retained body glyphs')
            modes[mode]['blur_edge_energy'] = edge_energy(blurred)
            modes[mode]['original_edge_energy'] = edge_energy(original)
        if mode in ('minimized', 'blurred', 'hidden'):
            assert secret.casefold() not in rendered.casefold(), (mode, rendered)
            copied = _policy_drag_copy(ctx, (left[0], (left[1] + left[3]) / 2),
                                      (left[2] + 180, (left[1] + left[3]) / 2), forbidden=secret)
            modes[mode]['unrevealed_clipboard'] = copied
        modes[mode]['search_words'] = _policy_search_privacy(ctx, secret, mode, evidence=modes[mode])
        if mode == 'minimized':
            summaries = [(text, box) for text, box in words if 'minimized' in text.casefold()]
            assert len(summaries) == 1, ('Missing actual minimized group', words)
            group_size = sum(row[0] == ctx['query_id'] and row[1] == ctx['query_target']
                             for row in ctx['expected_messages'])
            assert any(text == str(group_size) for text, other in words
                       if abs(other[1] - summaries[0][1][1]) < 8), (
                'Retained fool messages were not grouped into one native summary', words)
            modes[mode]['group_size'] = group_size
            box = summaries[0][1]
            gui.click(ctx['window'], (box[0] + box[2]) / 2, (box[1] + box[3]) / 2)
        elif mode == 'blurred':
            gui.click(ctx['window'], left[2] + 10, (left[1] + left[3]) / 2)
        elif mode == 'hidden':
            assert not gui.native_marker_components(gui.native_rgb_capture(ctx['window']),
                gui.read_layout(ctx['preferences'])['conversation']['rect'], rgb), 'Hidden row still rendered'
            continue
        # Reveal/dim is not accepted on a screenshot alone: select the original
        # decoded bytes between the two real marker rectangles.
        def revealed_pair():
            image = gui.native_rgb_capture(ctx['window'])
            rect = gui.read_layout(ctx['preferences'])['conversation']['rect']
            pairs = [gui.native_marker_components(image, rect, color)
                     for color in (rgb, (179, 79, 213))]
            if any(not items for items in pairs):
                return False
            return [max((item['bbox'] for item in items), key=lambda box: box[1]) for items in pairs]
        if mode == 'dimmed':
            assert secret.casefold() in rendered.casefold(), ('Dimmed body is not readable', rendered)
            # Copy through real row context works even with alpha-composited ink.
            popup = gui.open_native_popup(ctx['window'], lambda: (
                gui.move_pointer(ctx['window'], left[2] + 15, (left[1] + left[3]) / 2),
                gui.POINTER.button(True, code=273), gui.POINTER.button(False, code=273)))
            gui.popup_keys(ctx['window'], popup, 'Home', 'Return', root_changed=False)
            decoded = 'LEFTLEFT ' + secret + ' café e\u0301 中 RGHTRGHT second'
            gui.SESSION.until('readable dim row copies original body', lambda: gui.clipboard_text() == decoded)
            modes[mode]['copied_text'] = gui.clipboard_text()
            popup = gui.open_native_popup(ctx['window'], lambda: (
                gui.move_pointer(ctx['window'], left[2] + 15, (left[1] + left[3]) / 2),
                gui.POINTER.button(True, code=273), gui.POINTER.button(False, code=273)))
            menu = _policy_words(ctx, 'fool-reply-menu')
            assert any(word.casefold() == 'reply' for word, _ in menu), (
                'Fixture lacks real referenceable Reply action needed for preview privacy', menu)
            gui.popup_keys(ctx['window'], popup, 'Home', 'Down', 'Return')
            gui._composer_native(ctx['window'], '', 0, 0)
            reply_region = _policy_reply_region(ctx, 'fool-reply-readable')
            preview = _policy_words(ctx, 'fool-reply-readable', reply_region)
            assert secret.casefold() in ' '.join(word for word, _ in preview).casefold(), (
                'Actual readable reply preview never showed retained source body', preview)
            _policy_appearance(ctx, fool='blurred')
            preview = _policy_words(ctx, 'fool-reply-remasked',
                                    _policy_reply_region(ctx, 'fool-reply-remasked'))
            assert secret.casefold() not in ' '.join(word for word, _ in preview).casefold(), (
                'Reply preview leaked source body after fool policy invalidated readability', preview)
            modes[mode]['masked_reply_words'] = preview
            gui.cancel_native_reply(ctx['window'], ctx['destination'],
                                    label='fool-reply-cancel', expected_text='')
        else:
            a, b = gui.SESSION.until('revealed original colored body markers', revealed_pair)
            _policy_drag_copy(ctx, (a[2] + .5, (a[1] + a[3]) / 2),
                              (b[0] - .5, (b[1] + b[3]) / 2),
                              expected=' ' + secret + ' café e\u0301 中 ')
            modes[mode]['revealed_markers'] = [a, b]
            modes[mode]['copied_text'] = gui.clipboard_text()
        _policy_capture(ctx, 'fools-' + mode + '-readable')
    # Leave query unread in background. Same read marker must survive changing
    # count policy; recount must recover the real retained message, not drop it.
    _policy_appearance(ctx, fool='hidden', unread=False)
    gui.wait_read_marker(ctx['proc'], ctx['database_path'], ctx['query_id'])
    _policy_focus(ctx, ctx['channel_id'])
    before = gui.save_state(ctx['proc'], ctx['preferences'], ctx['database_path'])
    row_before = next(row for row in before[2] if row[0] == ctx['query_id'])
    new = 'MutedFoolUnread ' + str(time.monotonic_ns())
    _policy_send(ctx, new)
    _policy_typing(ctx, 'active')
    after = gui.save_state(ctx['proc'], ctx['preferences'], ctx['database_path'])
    row_after = next(row for row in after[2] if row[0] == ctx['query_id'])
    assert row_after[1:3] == row_before[1:3], (row_before, row_after)
    _policy_appearance(ctx, unread=True)
    gui.wait_rows(ctx['proc'], ctx['database_path'], 'SELECT unread,read_marker FROM buffers WHERE id=?',
                  (ctx['query_id'],), [(row_before[1] + 1, row_before[2])])
    _policy_appearance(ctx, unread=False)
    gui.wait_rows(ctx['proc'], ctx['database_path'], 'SELECT unread,read_marker FROM buffers WHERE id=?',
                  (ctx['query_id'],), [(row_before[1], row_before[2])])
    _policy_focus(ctx, ctx['query_id'])
    w = ctx['window']
    layout = gui.read_layout(ctx['preferences'])
    scale = layout['scale']
    x, y, width, height = layout['conversation']['rect']
    metrics = gui.pane_metrics(layout, scale)
    _, debug, root = gui.GUI_DEBUG[w]
    footer_frames, footer_images = {}, {}
    for phase in ('before', 'after'):
        if phase == 'after':
            _policy_typing(ctx, 'active')
            # Muted TAGMSG need not invalidate the UI. Observe the next real
            # root paint after its genuine echo, without changing the Input.
            marker = debug.stat().st_size
            gui.await_native_frame(w, marker,
                re.compile(rf'wl_surface[@#]{root}\.attach\(wl_buffer[@#]\d+,'))
        frame = gui.native_rgb_capture(w)
        native = Image.frombytes('RGB', (frame['width'], frame['height']), frame['pixels'])
        caret = gui.native_input_rectangle(w)
        assert caret is not None, 'Muted typing proof requires the actual visible Input'
        column = round(x + width - 200 * scale)
        row = round(caret[1] + caret[3] / 2)
        canvas = native.getpixel((column, round(y + height / 2)))
        assert native.getpixel((column, row)) != canvas, 'Input scan did not start inside its paint'
        # Scan the blank Input column, not font metrics or placeholder glyphs.
        while row > y + metrics['top'] and native.getpixel((column, row)) != canvas:
            row -= 1
        composer_top = row + 1
        bottom = round(composer_top - metrics['padding_y'] - metrics['gap'])
        region = (round(x + metrics['left']), round(bottom - 32 * scale),
                  round(x + width - metrics['left']), bottom)
        assert (x <= region[0] < region[2] <= x + width and
                y + metrics['top'] < region[1] < region[3] < composer_top <= caret[1]), (
                    'Invalid native reserved typing footer', region, composer_top, caret)
        path = Path(str(ctx['destination']).removesuffix('.bmp') + '-fools-no-typing-' + phase)
        native.save(str(path) + '.png')
        footer = native.crop(region)
        footer.save(str(path) + '-footer.png')
        footer_images[phase] = footer
        footer_frames[phase] = {'native_png': str(path) + '.png',
            'footer_png': str(path) + '-footer.png', 'footer_ltrb': region,
            'composer_top': composer_top, 'caret': caret, 'scan_column': column,
            'canvas_rgb': canvas}
    typing_evidence = {'nick': ctx['query_target'], 'frames': footer_frames,
                       'post_echo_log_offset': marker, 'root_surface': root}
    evidence_path = Path(str(ctx['destination']).removesuffix('.bmp') + '-fools-no-typing.json')
    evidence_path.write_text(json.dumps(typing_evidence, indent=2) + '\n')
    words, chip = _policy_chip_words(ctx, native, region, caret[3],
                                    'fools-no-typing', allow_absent=True)
    typing_evidence.update({'recognized_footer_words': words, 'chip_recognition': chip})
    evidence_path.write_text(json.dumps(typing_evidence, indent=2) + '\n')
    assert not any(text.casefold() == ctx['query_target'].casefold() for text, _ in words), (
        'Muted fool typing appeared in reserved footer', words, region)
    assert (footer_frames['before']['footer_ltrb'] == region and
            footer_images['before'].tobytes() == footer_images['after'].tobytes()), (
                'Muted TAGMSG changed the actual reserved footer', typing_evidence)
    gui.wait_messages(ctx['proc'], ctx['database_path'], ctx['expected_messages'])
    return {'modes': modes, 'unread_before': row_before, 'unread_muted': row_after,
            'unread_opt_in_expected': row_before[1] + 1, 'muted_typing_words': words,
            'muted_typing_footer': typing_evidence, 'muted_typing_evidence': str(evidence_path),
            'retained_expected_messages': list(ctx['expected_messages'])}


def run_transcript(ctx):
    """80 graphemes, native row styles, unboxed inline nick, exact original copy."""
    _policy_focus(ctx, ctx['query_id'])
    gui.set_navigation(ctx['proc'], ctx['window'], ctx['preferences'], ctx['database_path'],
                       'hidden', 'policy-wrap-wide')
    gui.set_people(ctx['proc'], ctx['window'], ctx['preferences'], ctx['database_path'], 'hidden')
    _policy_effects(ctx, fx='off')
    _policy_chat(ctx, fools=[])
    _policy_appearance(ctx, wrap=80)
    left_color, right_color = (42, 185, 113), (179, 79, 213)
    middle = 'e\u0301' * 72
    raw = '\x042AB971,2AB971LEFTLEFT\x0f' + middle + '\x04B34FD5,B34FD5RGHTRGHT\x0f'
    _policy_send(ctx, raw)
    wrapping = {}
    for style in ('bubbles', 'irc', 'stacked'):
        gui.set_message_style(ctx['proc'], ctx['window'], ctx['preferences'], ctx['database_path'],
                              ctx['args'], ctx['destination'], ctx['query_id'], style,
                              label='policy-wrap-' + style)
        _policy_appearance(ctx, wrap=0)
        a, b = _policy_wait_box(ctx, left_color), _policy_wait_box(ctx, right_color)
        assert abs(a[1] - b[1]) <= 1, ('Unlimited reference is viewport-wrapped; enlarge fixture pane', a, b)
        unlimited = [a, b]
        _policy_appearance(ctx, wrap=80)
        a, b = _policy_wait_box(ctx, left_color), _policy_wait_box(ctx, right_color)
        assert b[1] > a[3] - 1 and abs(b[0] - a[0]) <= 2, ('Not wrapped at 80 graphemes', a, b)
        # 8 marker graphemes +72 combining graphemes consume exactly80;
        # the next colored marker starts the next row. No copied newline.
        _policy_drag_copy(ctx, (a[2] + .5, (a[1] + a[3]) / 2),
                          (b[0] - .5, (b[1] + b[3]) / 2), expected=middle)
        wrapping[style] = {'unlimited_markers': unlimited, 'wrap80_markers': [a, b],
                           'copied_text': gui.clipboard_text()}
        _policy_capture(ctx, 'wrap80-' + style)
    _policy_focus(ctx, ctx['channel_id'])
    nick = ctx['query_target']
    text = '\x042AB971,2AB971LEFTLEFT\x0f ' + nick + ' \x04B34FD5,B34FD5RGHTRGHT\x0f'
    _policy_send(ctx, text, target=ctx['channel'])
    a, b = _policy_wait_box(ctx, left_color), _policy_wait_box(ctx, right_color)
    assert abs(a[1] - b[1]) <= 1, (a, b)
    image = gui.native_rgb_capture(ctx['window'])
    # The nickname is isolated between real body swatches; inspect padding
    # outside their glyph-height band instead of OCR-benchmarking a technical nick.
    def rgb(x, y):
        index = (round(y) * image['width'] + round(x)) * 3
        return image['pixels'][index:index + 3]
    padding = []
    for y in (round(min(a[1], b[1]) - 1), round(max(a[3], b[3]) + 1)):
        reference = rgb(a[2] + 2, y)
        row = [rgb(x, y) for x in range(round(a[2] + 2), round(b[0] - 2))]
        assert row, (a, b)
        assert all(max(abs(v - r) for v, r in zip(pixel, reference)) <= 2 for pixel in row), \
            'Inline nickname paints a chip/background outside the ordinary body line'
        padding.append(list(reference))
    mention_capture = _policy_capture(ctx, 'inline-nick-no-chip')
    _policy_drag_copy(ctx, (a[2] + .5, (a[1] + a[3]) / 2),
                      (b[0] - .5, (b[1] + b[3]) / 2), expected=' ' + nick + ' ')
    gui.wait_messages(ctx['proc'], ctx['database_path'], ctx['expected_messages'])
    return {'wrapping': wrapping, 'inline_markers': [a, b],
            'copied_inline_text': gui.clipboard_text(), 'padding_rgb': padding,
            'inline_capture': str(mention_capture),
            'limits': 'Native transcript checks own bold/nick-color semantics; no OCR nickname benchmark.'}


def run_typing(ctx):
    """Real TAGMSG drives a quiet fixed footer; receive/FX/reduced motion honored."""
    _policy_focus(ctx, ctx['query_id'])
    gui.set_message_style(ctx['proc'], ctx['window'], ctx['preferences'], ctx['database_path'],
                          ctx['args'], ctx['destination'], ctx['query_id'], 'irc',
                          label='typing-native-nick-color')
    _policy_chat(ctx, fools=[], receive=True)
    _policy_typing_cap(ctx)
    _policy_effects(ctx)
    color = (42, 185, 113)
    _policy_send(ctx, '\x042AB971,2AB971ANCHORANCHOR\x0f stable transcript')
    anchor = _policy_wait_box(ctx, color)
    w = ctx['window']
    _policy_typing(ctx, 'done')
    gui.framed_keys(w, 'Left', 'Right')
    caret = gui.native_input_rectangle(w)
    before = gui.native_rgb_capture(w)
    region = gui.read_layout(ctx['preferences'])['conversation']['rect']
    footer = (region[0], anchor[3] + 1, region[2], max(1, caret[1] - anchor[3] - 1))
    _policy_typing(ctx, 'active')
    pill_observations = 0
    typing_started = time.monotonic()
    mid_active_retained = False
    retained_typing_frames = []
    def pill():
        nonlocal pill_observations, mid_active_retained
        elapsed_ms = round((time.monotonic() - typing_started) * 1000)
        if pill_observations == 0:
            label = 'typing-active-first'
        elif not mid_active_retained and elapsed_ms >= 2000:
            label = f'typing-active-mid-{elapsed_ms}ms'
            mid_active_retained = True
        elif pill_observations < 5:
            label = f'typing-active-early-{pill_observations}-{elapsed_ms}ms'
        else:
            label = 'typing-active'
        if label != 'typing-active':
            retained_typing_frames.append({'label': label, 'after_active_ms': elapsed_ms})
        pill_observations += 1
        capture = _policy_capture(ctx, label)
        words = _policy_words(ctx, label + '-bare',
                              (footer[0], footer[1], footer[2], footer[3]))
        evidence = {'words': words, 'bare_nickname': True}
        if label != 'typing-active':
            retained_typing_frames[-1]['chip_recognition'] = evidence
        found = [box for text, box in words
                 if text.casefold() == ctx['query_target'].casefold()]
        return found[-1] if found else False
    nick = gui.SESSION.until('actual footer typing nickname', pill)
    assert nick[1] < caret[1] and nick[3] - nick[1] < 28 * ctx['args'].zoom, nick
    assert _policy_wait_box(ctx, color) == anchor, 'Typing moved transcript/scroll anchor'
    assert gui.native_input_rectangle(w) == caret, 'Typing moved composer'
    active = gui.native_rgb_capture(w)
    # Dots are a second line now; sample the real reserved footer, not the
    # obsolete horizontal strip to the right of the nickname.
    pill_box = (footer[0], footer[1], footer[0] + footer[2], footer[1] + footer[3])
    assert _policy_crop(before, pill_box) != _policy_crop(active, pill_box), 'Typing pill has no actual pixels'
    # The nickname's surrounding footer remains ordinary background: a filled
    # chip would alter these horizontal padding rows outside its visible ink.
    padding = (max(0, nick[0] - 3), max(0, nick[1] - 3),
               min(active['width'], nick[2] + 3), min(active['height'], nick[3] + 3))
    for y in (padding[1], padding[3] - 1):
        band = (padding[0], y, padding[2], y + 1)
        assert _policy_crop(before, band) == _policy_crop(active, band), (
            'Typing nickname still paints chip padding', nick, band)
    reference_names = [box for text, box in _policy_words(ctx, 'typing-nick-color-reference')
                       if text.rstrip(':').casefold() == ctx['query_target'].casefold()
                       and box[3] < footer[1]]
    assert reference_names, 'No actual transcript author label for native nick-color comparison'
    def colored_ink(box):
        pixels = _policy_crop(active, box)
        return {tuple(pixels[index:index + 3]) for index in range(0, len(pixels), 3)
                if max(pixels[index:index + 3]) - min(pixels[index:index + 3]) >= 25}
    typing_ink = colored_ink(nick)
    author_ink = set().union(*(colored_ink(box) for box in reference_names))
    assert typing_ink & author_ink, (
        'Bare typing nickname does not share actual transcript nick-color ink',
        nick, reference_names, sorted(typing_ink), sorted(author_ink))
    samples = []
    until = time.monotonic() + 1.2
    while time.monotonic() < until:
        samples.append(_policy_crop(gui.native_rgb_capture(w), pill_box))
    assert len(set(samples)) > 1, 'Enabled slow typing pulse never changes native frames'
    pulse = {'samples': len(samples), 'distinct_frames': len(set(samples))}
    suppression = []
    for reduced, fx in ((True, 'full'), (False, 'off')):
        _policy_effects(ctx, reduced=reduced, fx=fx)
        _policy_typing(ctx, 'active')
        gui.SESSION.until('static typing nickname', pill)
        samples = []
        until = time.monotonic() + .8
        while time.monotonic() < until:
            samples.append(_policy_crop(gui.native_rgb_capture(w), pill_box))
        assert len(set(samples)) == 1, ('Suppressed typing pill animated', reduced, fx)
        suppression.append({'reduced_motion': reduced, 'fx': fx, 'samples': len(samples),
                            'distinct_frames': len(set(samples))})
    _policy_typing(ctx, 'done')
    gui.SESSION.until('done removes actual footer nickname', lambda: not pill())
    assert _policy_wait_box(ctx, color) == anchor and gui.native_input_rectangle(w) == caret
    # The authoritative expiry must also clear a real active event without done.
    _policy_typing(ctx, 'active')
    gui.SESSION.until('typing before expiry', pill)
    expiry_started = time.monotonic()
    deadline = time.monotonic() + 15
    while pill():
        assert time.monotonic() < deadline, 'Typing indicator did not expire'
        time.sleep(.2)
    expired_after = time.monotonic() - expiry_started
    _policy_chat(ctx, receive=False)
    _policy_typing(ctx, 'active')
    assert not pill(), 'typing.receive=false still displays actual peer activity'
    _policy_capture(ctx, 'typing-done')
    return {'transcript_anchor': anchor, 'composer_caret': caret, 'typing_nick_rect': nick,
            'pill_rect': pill_box, 'pulse': pulse, 'suppression': suppression,
            'retained_early_frames': retained_typing_frames,
            'expiry_observed_seconds': expired_after, 'receive_disabled_visible': bool(pill())}

def run_typing_bubble(ctx):
    """Four genuine channel TAGMSG senders, shared tray and passive scene popup."""
    from contextlib import ExitStack

    p, w, prefs, db = ctx['proc'], ctx['window'], ctx['preferences'], ctx['database_path']
    identity, channel = ctx['channel_id'], ctx['channel']
    _policy_focus(ctx, identity)
    _policy_chat(ctx, fools=[], receive=True)
    _policy_effects(ctx, fx='off')
    _policy_typing_cap(ctx)
    # Keep names and the popup in the conversation, not duplicated in People.
    gui.set_people(p, w, prefs, db, 'hidden')
    names = ('Amber', 'Cedar', 'Maple', 'Birch')
    color = (42, 185, 113)
    evidence = []
    with ExitStack() as stack:
        peers = []
        for nick in names:
            peer = stack.enter_context(gui.fixtures.BootstrapClient(
                dict(ctx['lab'].endpoints['direct'], nick=nick, username=nick),
                ctx['lab'].ca_file, authenticate=False))
            peer.send('CAP REQ :message-tags')
            _, ack, _ = peer.wait(lambda command, params, prefix: command == 'CAP' and
                                  ('ACK' in params or 'NAK' in params), timeout=15)
            assert 'ACK' in ack and 'message-tags' in ack[-1].split(), ack
            peer.send('JOIN ' + channel)
            gui.receive_until(ctx['observer'], 'JOIN', [channel], nick,
                              redact=ctx['lab']._redact)
            peers.append(peer)

        def typing(count):
            for index, peer in enumerate(peers):
                state = 'active' if index < count else 'done'
                peer.send(f'@+typing={state} TAGMSG {channel}')
                ctx['observer'].wait(
                    lambda command, params, prefix, nick=names[index], state=state:
                    command == 'TAGMSG' and params == [channel] and
                    prefix.split('!', 1)[0] == nick and
                    ctx['observer'].last_tags.get('+typing') == state, timeout=15)

        _policy_send(ctx, '\x042AB971,2AB971ANCHORANCHOR\x0f typing tray anchor',
                     target=channel)
        wire_start = len(ctx['observer'].frames)
        for style in ('bubbles', 'irc', 'stacked'):
            typing(0)
            gui.set_message_style(p, w, prefs, db, ctx['args'], ctx['destination'],
                                  identity, style, label='typing-bubble-' + style)
            empty_initial = gui._composer_native(w, '', 0, 0)
            gui.type_text('private café draft')
            draft = 'private café draft'
            size = len(draft.encode('utf-8'))
            initial = gui._composer_native(w, draft, size, size)
            anchor = _policy_wait_box(ctx, color)
            caret = initial['caret']
            pane = gui.read_layout(prefs)['conversation']['rect']
            footer = (pane[0], anchor[3] + 1, pane[2], caret[1] - anchor[3] - 1)
            assert footer[3] > 0, (footer, anchor, caret)
            phases = []

            def stable(label):
                assert _policy_wait_box(ctx, color) == anchor, (
                    'Typing changed the actual message anchor', label)
                assert gui._composer_native(w, draft, size, size) == initial, (
                    'Typing changed native draft/caret/selection', label)
                phases.append(label)

            def footer_words(label):
                return _policy_words(ctx, 'typing-bubble-' + style + '-' + label, footer)

            typing(1)
            single = gui.SESSION.until('one real typing identity in native tray', lambda:
                next((box for text, box in footer_words('one')
                      if text.rstrip(',').casefold() == names[0].casefold()), None))
            stable('one')
            typing(4)

            def shared():
                typing(4)  # Refresh real activity while native capture/OCR waits.
                words = footer_words('many')
                shown = [(text.rstrip(','), box) for text, box in words
                         if text.rstrip(',').casefold() in {nick.casefold() for nick in names}]
                # The representative wide scope must prove both names; cramped
                # optional geometry may legitimately reserve room for fewer.
                expected_counts = (2,) if pane[2] / ctx['args'].zoom >= 600 else (0, 1, 2)
                overflow = [box for text, box in words
                            if len(shown) in expected_counts and text == '+' + str(4 - len(shown))]
                return (shown, overflow[0]) if overflow else None

            shown, overflow = gui.SESSION.until('shared inline identities and exact remaining +N', shared)
            stable('many')
            tray_top = min([overflow[1], *(box[1] for _, box in shown)])
            method = ctx['input_method']
            assert method is not None, 'Focused scope requires its existing real NativeInputMethod'
            gui.SESSION.until('native IME owns unchanged draft', lambda:
                method.drain() is None and method.state['active'] and
                method.state['surrounding'] == (draft, size, size))
            trace = gui.GUI_DEBUG[w][1]
            marker = trace.stat().st_size
            method.transaction('猫', 3, 3)
            gui.await_native_frame(w, marker,
                re.compile(r'zwp_text_input_v3[@#]\d+\.preedit_string\('))
            preedit = gui._composer_native(w, draft, size, size)
            left, top, _, height = caret
            glyph_box = (left + 2, top, preedit['caret'][0] - 2, top + height)
            assert glyph_box[0] < glyph_box[2], (caret, preedit)
            glyphs = _policy_crop(gui.native_rgb_capture(w), glyph_box)
            passive_marker = trace.stat().st_size

            def retained():
                assert gui._composer_native(w, draft, size, size) == preedit
                method.drain()
                assert method.state['active'] and method.state['surrounding'] == (draft, size, size)
                assert _policy_crop(gui.native_rgb_capture(w), glyph_box) == glyphs, \
                    'Passive typing popup lost actual IME preedit glyphs'
                assert _policy_wait_box(ctx, color) == anchor

            def popup_words(label):
                return _policy_words(ctx, 'typing-bubble-' + style + '-' + label,
                                     (pane[0], pane[1], pane[2], tray_top - pane[1]))

            def opened():
                typing(4)
                words = popup_words('popup')
                found = {text.casefold(): box for text, box in words}
                return words if {'typing', 'now', *(nick.casefold() for nick in names)} <= found.keys() else None

            def closed(label):
                return not any(text.casefold() == 'typing' for text, _ in popup_words(label))

            gui.click(w, (overflow[0] + overflow[2]) / 2,
                      (overflow[1] + overflow[3]) / 2)
            popup = gui.SESSION.until('scene popup contains all four, including inline names', opened)
            retained()
            # Stay inside the scaled transcript's left edge, outside the list
            # even when its panel occupies almost the entire narrow viewport.
            metrics = gui.pane_metrics(gui.read_layout(prefs), ctx['args'].zoom)
            gui.click(w, pane[0] + metrics['left'] + 4 * ctx['args'].zoom,
                      pane[1] + pane[3] / 2)
            gui.SESSION.until('outside transcript click dismisses typing popup',
                              lambda: closed('dismissed'))
            retained()
            passive_trace = trace.read_bytes()[passive_marker:]
            assert not re.search(rb'zwp_text_input_v3[@#]\d+\.disable\(', passive_trace), \
                'Passive typing popup disabled the native Input'
            assert not re.search(rb'get_popup\(new id xdg_popup', passive_trace), \
                'Typing list requested a native dropdown instead of a scene popup'
            marker = trace.stat().st_size
            method.transaction('', 0, 0, committed='猫')
            gui.await_native_frame(w, marker,
                re.compile(r'zwp_text_input_v3[@#]\d+\.commit_string\('))
            committed = gui._composer_native(w, draft + '猫', size + 3, size + 3)
            typing(4)
            gui.click(w, (overflow[0] + overflow[2]) / 2,
                      (overflow[1] + overflow[3]) / 2)
            gui.SESSION.until('reopened complete typing list before switch', opened)
            _policy_focus(ctx, ctx['query_id'])
            assert not any(text.casefold() == 'typing'
                           for text, _ in _policy_words(ctx, 'typing-bubble-switched-' + style)), \
                'Typing popup survived its source buffer switch'
            _policy_focus(ctx, identity, expected=(draft + '猫', size + 3, size + 3))
            gui.SESSION.until('returning to source does not reopen stale typing popup',
                              lambda: closed('returned'))
            gui.clear_draft()
            empty_before_done = gui._composer_native(w, '', 0, 0)
            assert empty_before_done == empty_initial, \
                'Popup/switch changed the same empty-document native caret'
            typing(0)
            gui.SESSION.until('done clears all actual tray identities', lambda:
                not any(text.rstrip(',').casefold() in {nick.casefold() for nick in names}
                        or re.fullmatch(r'\+[234]', text) for text, _ in footer_words('done')))
            assert _policy_wait_box(ctx, color) == anchor
            empty_after_done = gui._composer_native(w, '', 0, 0)
            assert empty_after_done == empty_before_done, \
                'Done changed the same empty-document native caret'
            evidence.append({'style': style, 'anchor': anchor, 'caret': caret,
                'empty_initial': empty_initial, 'empty_before_done': empty_before_done,
                'empty_after_done': empty_after_done,
                'stable_phases': ['zero', *phases, 'done'], 'single': single,
                'inline_names': shown, 'overflow': overflow, 'popup_words': popup,
                'preedit': preedit, 'preedit_glyph_box': glyph_box, 'committed': committed,
                'no_native_popup_or_input_disable': True,
                'capture': str(_policy_capture(ctx, 'typing-bubble-final-' + style))})
        assert not [frame for frame in ctx['observer'].frames[wire_start:]
                    if frame['command'] == 'PRIVMSG' and
                    frame['prefix'].split('!', 1)[0] == ctx['own_nick']], \
            'Typing popup/draft/IME unexpectedly sent a real message'
    return _refresh_return(ctx, 'typing-bubble', {
        'eligible': names, 'phases': evidence,
        'limitations': ['Native Wayland exposes Input ownership/caret, not scene nodes. '
            'Existing native raster text recognition locates +2 and the read-only list; '
            'scene avatar count, exact tray height, colors and surfaces belong to native C++ proofs.']})



def run_incoming(ctx):
    """Sample real row frames during live delivery, not before/after unrelated UI."""
    defaults = tomllib.loads(Path(ctx['preferences']).read_text())['appearance']
    assert (defaults['incoming_motion'], defaults['incoming_reveal'],
            defaults['incoming_duration_ms']) == ('none', 'fade', 550), defaults
    _policy_focus(ctx, ctx['query_id'])
    _policy_chat(ctx, fools=[])
    # Each independent movement and reveal is exercised; capture begins before
    # durable storage waits so a fast machine cannot miss the whole timeline.
    cases = [('none', 'none', 'full', False)]
    cases += [(motion, 'none', 'full', False) for motion in ('rise', 'pop', 'slide', 'drift')]
    cases += [('none', reveal, 'full', False) for reveal in ('fade', 'pixels')]
    cases += [('slide', 'pixels', 'off', False), ('slide', 'pixels', 'full', True)]
    evidence = []
    for index, (motion, reveal, fx, reduced) in enumerate(cases):
        _policy_effects(ctx, motion=motion, reveal=reveal, fx=fx, reduced=reduced)
        gui.move_pointer(ctx['window'], 5, 5)
        color = (41 + index * 17, 173 - index * 9, 71 + index * 11)
        token = ''.join(f'{component:02X}' for component in color)
        text = f'\x04{token},{token}INCOMING{index}INCOMING\x0f live policy {index}'
        _policy_send(ctx, text, durable=False)
        frames = []
        deadline = time.monotonic() + 2.6
        while time.monotonic() < deadline:
            capture = gui.native_rgb_capture(ctx['window'])
            frames.append((time.monotonic(), capture, _policy_box(ctx, color, image=capture)))
        gui.wait_messages(ctx['proc'], ctx['database_path'], ctx['expected_messages'])
        settled = _policy_wait_box(ctx, color)
        settled_capture = gui.native_rgb_capture(ctx['window'])
        # Compare only this message's solid body swatch; timestamps, pointer,
        # composer caret, other rows and status animations cannot satisfy it.
        box = (max(0, settled[0] - 48), max(0, settled[1] - 32),
               min(settled_capture['width'], settled[2] + 48), settled[3] + 32)
        visible = [(stamp, image, marker) for stamp, image, marker in frames if marker is not None]
        assert visible, ('Live incoming swatch was never rendered', motion, reveal)
        first = visible[0][0]
        observed = [_policy_crop(image, box) for _, image, _ in frames]
        enabled = fx != 'off' and not reduced and (motion != 'none' or reveal != 'none')
        if enabled:
            assert len(set(observed)) >= 3, ('Configured incoming effect did not animate real row', motion, reveal)
            if motion in ('slide', 'drift', 'rise'):
                assert any(marker != settled for _, _, marker in visible), (
                    'Incoming movement did not move native marker geometry', motion, visible[0][2], settled)
        else:
            # Permit the first root paint to settle, but not an animation tail.
            stable = [_policy_crop(image, box) for stamp, image, _ in visible if stamp >= first + .15]
            assert len(stable) >= 2 and len(set(stable)) == 1, (
                'None/FX Off/Reduced motion still animated incoming body', motion, reveal, fx, reduced)
        final = _policy_crop(settled_capture, box)
        assert observed[-1] == final, 'Incoming row did not settle by configured duration'
        evidence.append({'motion': motion, 'reveal': reveal, 'fx': fx, 'reduced_motion': reduced,
                         'frames': len(frames), 'distinct_body_frames': len(set(observed)),
                         'distinct_marker_rects': len(set(tuple(marker) for _, _, marker in visible)),
                         'settled_marker': settled, 'settled_crop_matches_last_frame': observed[-1] == final})
        _policy_capture(ctx, f'incoming-{index}-{motion}-{reveal}-{fx}-{reduced}')
    # Dim is explicitly readable, so it gives an observable body on which to
    # prove fool suppression rather than trivially testing an invisible row.
    _policy_chat(ctx, fools=[ctx['query_target']])
    _policy_appearance(ctx, fool='dimmed')
    _policy_effects(ctx, motion='slide', reveal='pixels')
    gui.move_pointer(ctx['window'], 5, 5)
    started = time.monotonic()
    _policy_send(ctx, 'MutedMotionProbe café retained fool', durable=False)
    frames = []
    while time.monotonic() - started < 2.6:
        frames.append((time.monotonic(), gui.native_rgb_capture(ctx['window'])))
    words = _policy_words(ctx, 'incoming-muted-fool')
    boxes = [box for text, box in words if text.casefold() == 'mutedmotionprobe']
    assert len(boxes) == 1, ('Readable dim fool body missing', words)
    box = boxes[0]
    final = _policy_crop(gui.native_rgb_capture(ctx['window']), box)
    cropped = [(stamp, _policy_crop(image, box)) for stamp, image in frames]
    first_complete = next((stamp for stamp, pixels in cropped if pixels == final), None)
    assert first_complete is not None and first_complete - started < .75, (
        'Fool body incorrectly entered the configured 2000ms incoming timeline', first_complete, started)
    assert all(pixels == final for stamp, pixels in cropped if stamp >= first_complete), (
        'Fool activity animated native body pixels')
    gui.wait_messages(ctx['proc'], ctx['database_path'], ctx['expected_messages'])
    return {'incoming_cases': evidence,
            'muted_fool': {'body_rect': box, 'frames': len(frames),
                           'first_complete_seconds': first_complete - started,
                           'distinct_body_frames_after_complete': len(set(
                               pixels for stamp, pixels in cropped if stamp >= first_complete))}}


# Append before the receiving interface_refresh_smoke.py __main__ guard.
# Dependencies: existing gui/upload helpers and Pillow; stdlib ctypes, hashlib,
# json, queue, threading, re, pathlib; installed native GIO/GLib/GObject and notify-send.
import ctypes
import hashlib
import json
import queue
import threading
import re
import tomllib
from PIL import Image
from pathlib import Path


class _RefreshNotificationService:
    """Actual freedesktop service on an explicit owned bus, never the cached session bus."""
    name = b'org.freedesktop.Notifications'
    path = b'/org/freedesktop/Notifications'
    xml = b'''<node><interface name="org.freedesktop.Notifications">
      <method name="GetCapabilities"><arg type="as" direction="out"/></method>
      <method name="GetServerInformation"><arg type="s" direction="out"/><arg type="s" direction="out"/><arg type="s" direction="out"/><arg type="s" direction="out"/></method>
      <method name="Notify"><arg type="s" direction="in"/><arg type="u" direction="in"/><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="as" direction="in"/><arg type="a{sv}" direction="in"/><arg type="i" direction="in"/><arg type="u" direction="out"/></method>
      <method name="CloseNotification"><arg type="u" direction="in"/></method>
      <signal name="NotificationClosed"><arg type="u"/><arg type="u"/></signal>
      <signal name="ActionInvoked"><arg type="u"/><arg type="s"/></signal>
    </interface></node>'''

    def __init__(self, address):
        assert address.startswith('unix:'), 'Notification service requires an owned Unix bus'
        self.address = address
        self.records, self.errors = queue.Queue(), queue.Queue()
        self.ready, self.stop = threading.Event(), threading.Event()
        self.methods = []
        self.active, self.next_id = set(), 1
        self.owned = False

    def __enter__(self):
        self.thread = threading.Thread(target=self._serve, name='private-notifications', daemon=True)
        self.thread.start()
        try:
            assert self.ready.wait(15), 'Private notification service registration timed out'
            self.raise_errors()
            assert self.owned, 'Private notification name was not acquired before restart'
            return self
        except BaseException:
            self.__exit__()
            raise

    def raise_errors(self):
        if not self.errors.empty():
            raise AssertionError('Private notification service failed') from self.errors.get_nowait()

    def __exit__(self, *exc):
        self.stop.set()
        self.thread.join(15)
        assert not self.thread.is_alive(), 'Private notification service failed to stop'
        self.raise_errors()

    def _serve(self):
        context = connection = node = registration = None
        pushed = False
        try:
            p, c, u, i, z = (ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint,
                             ctypes.c_int, ctypes.c_size_t)
            self.gio = gui.native_library('gio-2.0', 'libgio-2.0.so.0')
            self.glib = gui.native_library('glib-2.0', 'libglib-2.0.so.0')
            self.gobject = gui.native_library('gobject-2.0', 'libgobject-2.0.so.0')

            def bind(lib, name, result, *args):
                fn = getattr(lib, name)
                fn.restype, fn.argtypes = result, list(args)
                return fn

            self.context_new = bind(self.glib, 'g_main_context_new', p)
            self.context_push = bind(self.glib, 'g_main_context_push_thread_default', None, p)
            self.context_pop = bind(self.glib, 'g_main_context_pop_thread_default', None, p)
            self.context_unref = bind(self.glib, 'g_main_context_unref', None, p)
            self.iterate = bind(self.glib, 'g_main_context_iteration', i, p, i)
            self.unref = bind(self.gobject, 'g_object_unref', None, p)
            self.free_error = bind(self.glib, 'g_error_free', None, p)
            self.free = bind(self.glib, 'g_free', None, p)
            self.v_unref = bind(self.glib, 'g_variant_unref', None, p)
            self.v_string = bind(self.glib, 'g_variant_new_string', p, c)
            self.v_uint = bind(self.glib, 'g_variant_new_uint32', p, u)
            self.v_strv = bind(self.glib, 'g_variant_new_strv', p, ctypes.POINTER(c), ctypes.c_ssize_t)
            self.v_tuple = bind(self.glib, 'g_variant_new_tuple', p, ctypes.POINTER(p), z)
            self.child = bind(self.glib, 'g_variant_get_child_value', p, p, z)
            self.lookup_value = bind(self.glib, 'g_variant_lookup_value', p, p, c, c)
            self.get_string = bind(self.glib, 'g_variant_get_string', c, p, ctypes.POINTER(z))
            self.get_uint = bind(self.glib, 'g_variant_get_uint32', u, p)
            self.get_int = bind(self.glib, 'g_variant_get_int32', i, p)
            self.get_bool = bind(self.glib, 'g_variant_get_boolean', i, p)
            self.signature = bind(self.glib, 'g_variant_get_type_string', c, p)
            self.print_variant = bind(self.glib, 'g_variant_print', p, p, i)
            self.connect = bind(self.gio, 'g_dbus_connection_new_for_address_sync', p,
                                c, u, p, p, ctypes.POINTER(p))
            self.close = bind(self.gio, 'g_dbus_connection_close_sync', i, p, p, ctypes.POINTER(p))
            self.node_new = bind(self.gio, 'g_dbus_node_info_new_for_xml', p, c, ctypes.POINTER(p))
            self.node_unref = bind(self.gio, 'g_dbus_node_info_unref', None, p)
            self.lookup = bind(self.gio, 'g_dbus_node_info_lookup_interface', p, p, c)
            self.call_sync = bind(self.gio, 'g_dbus_connection_call_sync', p,
                                  p, c, c, c, c, p, p, u, i, p, ctypes.POINTER(p))
            self.reply = bind(self.gio, 'g_dbus_method_invocation_return_value', None, p, p)
            self.reply_error = bind(self.gio, 'g_dbus_method_invocation_return_dbus_error', None, p, c, c)
            self.emit = bind(self.gio, 'g_dbus_connection_emit_signal', i,
                             p, c, c, c, c, p, ctypes.POINTER(p))
            callback_type = ctypes.CFUNCTYPE(None, p, c, c, c, c, p, p, p)

            class VTable(ctypes.Structure):
                _fields_ = [('method_call', callback_type), ('get_property', p),
                            ('set_property', p), ('padding', p * 8)]

            self.callback = callback_type(self._method)
            self.vtable = VTable(self.callback, None, None, (p * 8)())
            register = bind(self.gio, 'g_dbus_connection_register_object', u,
                            p, c, p, ctypes.POINTER(VTable), p, p, ctypes.POINTER(p))
            unregister = bind(self.gio, 'g_dbus_connection_unregister_object', i, p, u)
            context = self.context_new()
            assert context, 'Could not create owned GLib context'
            self.context_push(context)
            pushed = True
            error = p()
            connection = self.connect(self.address.encode(), 1 | 8, None, None, ctypes.byref(error))
            self._error(error)
            assert connection, 'Could not connect to private notification bus'
            self.connection = connection
            node = self.node_new(self.xml, ctypes.byref(error))
            self._error(error)
            assert node, 'Could not parse notification introspection'
            interface = self.lookup(node, self.name)
            registration = register(connection, self.path, interface, ctypes.byref(self.vtable),
                                    None, None, ctypes.byref(error))
            self._error(error)
            assert registration, 'Could not register notification interface'
            result = self._bus_call(b'RequestName', self._tuple(
                self.v_string(self.name), self.v_uint(4)))  # DO_NOT_QUEUE
            try:
                assert self._field(result, 0, self.get_uint) == 1, 'Notification name already owned'
            finally:
                self.v_unref(result)
            result = self._bus_call(b'NameHasOwner', self._tuple(self.v_string(self.name)))
            try:
                self.owned = bool(self._field(result, 0, self.get_bool))
                assert self.owned, 'Private bus did not confirm notification name ownership'
            finally:
                self.v_unref(result)
            self.ready.set()
            while not self.stop.is_set():
                # Only this thread dispatches this private context; no global GLib loop/cache.
                while not self.stop.is_set() and self.iterate(context, False):
                    pass
                self.stop.wait(.01)
        except BaseException as error:
            self.errors.put(error)
            self.ready.set()
        finally:
            try:
                if registration:
                    unregister(connection, registration)
                if connection:
                    error = ctypes.c_void_p()
                    self.close(connection, None, ctypes.byref(error))
                    self._error(error)
            except BaseException as error:
                self.errors.put(error)
            finally:
                if connection:
                    self.unref(connection)
                if node:
                    self.node_unref(node)
                if pushed:
                    self.context_pop(context)
                if context:
                    self.context_unref(context)

    def _error(self, error):
        if error.value:
            class GError(ctypes.Structure):
                _fields_ = [('domain', ctypes.c_uint), ('code', ctypes.c_int),
                            ('message', ctypes.c_char_p)]
            message = ctypes.cast(error, ctypes.POINTER(GError)).contents.message.decode()
            self.free_error(error)
            error.value = None
            raise RuntimeError(message)

    def _tuple(self, *values):
        return self.v_tuple((ctypes.c_void_p * len(values))(*values), len(values))

    def _field(self, value, index, convert):
        child = self.child(value, index)
        try:
            return convert(child)
        finally:
            self.v_unref(child)

    def _text(self, value):
        return self.get_string(value, None).decode('utf-8')

    def _image_path(self, hints):
        value = self.lookup_value(hints, b'image-path', b's')
        if not value:
            return None
        try:
            return self._text(value)
        finally:
            self.v_unref(value)

    def _printed(self, value):
        text = self.print_variant(value, True)
        try:
            return ctypes.string_at(text).decode('utf-8')
        finally:
            self.free(text)

    def _bus_call(self, method, parameters):
        error = ctypes.c_void_p()
        result = self.call_sync(self.connection, b'org.freedesktop.DBus',
            b'/org/freedesktop/DBus', b'org.freedesktop.DBus', method,
            parameters, None, 0, 5000, None, ctypes.byref(error))
        self._error(error)
        assert result, 'Private bus method returned no result'
        return result

    def _method(self, connection, sender, path, interface, method, parameters, invocation, data):
        try:
            self.methods.append(method.decode())
            if method == b'GetCapabilities':
                # No body-markup/actions advertised: the service consumes plain synthetic text.
                capabilities = (ctypes.c_char_p * 1)(b'body')
                result = self._tuple(self.v_strv(capabilities, 1))
            elif method == b'GetServerInformation':
                result = self._tuple(*(self.v_string(s) for s in
                    (b'BlastIRC private smoke', b'BlastIRC tests', b'1', b'1.2')))
            elif method == b'Notify':
                assert self.signature(parameters) == b'(susssasa{sv}i)'
                fields = [self._field(parameters, index, self._text) for index in (0, 2, 3, 4)]
                replaces = self._field(parameters, 1, self.get_uint)
                identity = replaces if replaces in self.active else self.next_id
                if identity == self.next_id:
                    self.next_id += 1
                self.active.add(identity)
                record = dict(zip(('app_name', 'app_icon', 'summary', 'body'), fields))
                record.update(notification_id=identity, replaces_id=replaces, sender=sender.decode(),
                    actions=self._field(parameters, 5, self._printed),
                    hints=self._field(parameters, 6, self._printed),
                    image_path=self._field(parameters, 6, self._image_path),
                    expire_timeout=self._field(parameters, 7, self.get_int))
                result = self._tuple(self.v_uint(identity))
                self.reply(invocation, result)
                self.records.put(record)
                return
            elif method == b'CloseNotification':
                identity = self._field(parameters, 0, self.get_uint)
                self.reply(invocation, self._tuple())
                if identity in self.active:
                    self.active.remove(identity)
                    error = ctypes.c_void_p()
                    assert self.emit(connection, None, self.path, self.name, b'NotificationClosed',
                        self._tuple(self.v_uint(identity), self.v_uint(3)), ctypes.byref(error))
                    self._error(error)
                return
            else:
                self.reply_error(invocation, b'org.freedesktop.DBus.Error.UnknownMethod', method)
                return
            self.reply(invocation, result)
        except BaseException as error:
            self.errors.put(error)
            self.reply_error(invocation, b'org.freedesktop.DBus.Error.Failed', str(error).encode())


def run_notifications(ctx):
    """Observe real notify-send Notify traffic without contacting any desktop service."""
    output = Path(ctx['destination']).with_name('refresh-private-notifications')
    output.mkdir(mode=0o700, exist_ok=True)
    previous_env = dict(ctx['env'])
    # Keep bus + consumer alive until the parent finishes its owned application.
    # ExitStack ordering stops the service before stopping its bus.
    ctx['cleanup'].enter_context(upload.DesktopServices(ctx['env'], output, None, None))
    ctx['cleanup'].callback(lambda: (ctx['env'].clear(), ctx['env'].update(previous_env)))
    service = ctx['cleanup'].enter_context(
        _RefreshNotificationService(ctx['env']['DBUS_SESSION_BUS_ADDRESS']))
    typing = tomllib.loads(Path(ctx['preferences']).read_text()).get('typing', {})
    _refresh_restart(ctx, 'typing', {key: json.dumps(value) for key, value in typing.items()})
    service.raise_errors()
    window = ctx['window']
    original_workspace = next(workspace['name'] for workspace in
                              gui.SESSION.ipc('get_workspaces') if workspace['focused'])
    workspace = 'refresh-notification-unfocused'
    assert original_workspace != workspace
    trace = gui.GUI_DEBUG[window][1]
    marker = trace.stat().st_size
    gui.SESSION.ipc('command', 'workspace "' + workspace + '"')
    try:
        def unfocused():
            service.raise_errors()
            assert ctx['proc'].poll() is None, 'Native app exited before notification'
            leave = re.search(r'wl_keyboard[@#]\d+\.leave\(\d+,\s*wl_surface[@#]\d+',
                              trace.read_bytes()[marker:].decode(errors='replace'))
            selected = any(item['name'] == workspace and item['focused']
                           for item in gui.SESSION.ipc('get_workspaces'))
            return bool(leave) and selected
        gui.SESSION.until('owned native keyboard leave before actual IRC mention', unfocused)
        body = (ctx['own_nick'] + ': synthetic notification <b>NOT MARKUP</b> & '
                '<img src="file:///synthetic-not-real"> "quoted" \'single\' café')
        ctx['observer'].send('PRIVMSG ' + ctx['own_nick'] + ' :' + body)
        row = (ctx['query_id'], ctx['query_target'], body, 0, 0)
        ctx['expected_messages'].append(row)

        def captured():
            service.raise_errors()
            assert ctx['proc'].poll() is None, 'Native app exited while delivering notification'
            try:
                return service.records.get_nowait()
            except queue.Empty:
                return None
        notification = gui.SESSION.until('actual notify-send D-Bus Notify', captured)
        assert notification['app_name'] == 'BlastIRC', notification
        expected_body = (body.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')
                        .replace('"', '&quot;').replace("'", '&apos;'))
        assert notification['body'] == expected_body, notification
        assert notification['summary'] == ctx['query_target'] + ' in ' + ctx['query_target'], notification
        assert notification['notification_id'] > 0 and notification['sender'].startswith(':'), notification
        # Freedesktop image-path takes precedence over app_icon for a single image.
        icon_path = notification['image_path'] or notification['app_icon']
        icon = Path(icon_path)
        assert icon.is_absolute() and icon.is_file(), notification
        assert str(icon.resolve(strict=True)) == icon_path, notification
        expected = gui.ROOT / 'assets' / 'icon.png'
        actual_bytes, expected_bytes = icon.read_bytes(), expected.read_bytes()
        actual_sha = hashlib.sha256(actual_bytes).hexdigest()
        expected_sha = hashlib.sha256(expected_bytes).hexdigest()
        assert actual_sha == expected_sha and actual_bytes == expected_bytes, (str(icon), str(expected))
        with Image.open(icon) as actual, Image.open(expected) as reference:
            assert actual.format == reference.format == 'PNG'
            assert actual.size == reference.size and min(actual.size) > 0
            pixels, expected_pixels = actual.convert('RGBA').tobytes(), reference.convert('RGBA').tobytes()
            assert pixels == expected_pixels, 'Captured notification image differs from current logo pixels'
            dimensions = list(actual.size)
        evidence = {'transport': 'native notify-send -> owned private D-Bus Notify -> GIO service',
            'service_owned_before_restart': service.owned, 'service_methods': list(service.methods),
            'notification': notification, 'synthetic_body': body,
            'expected_escaped_body': expected_body, 'body_markup_escaped': True,
            'canonical_icon_path': str(icon), 'repository_icon': str(expected.resolve()),
            'icon_sha256': actual_sha, 'repository_icon_sha256': expected_sha,
            'icon_byte_length': len(actual_bytes), 'icon_dimensions': dimensions,
            'rgba_sha256': hashlib.sha256(pixels).hexdigest(),
            'repository_rgba_sha256': hashlib.sha256(expected_pixels).hexdigest(),
            'rgba_pixels_equal': True, 'unfocused_workspace': workspace,
            'native_keyboard_leave_observed': True, 'host_bus_contacted': False}
    finally:
        gui.SESSION.ipc('command', 'workspace ' + json.dumps(original_workspace))
    gui.wait_messages(ctx['proc'], ctx['database_path'], ctx['expected_messages'])
    upload.focus_composer(window)
    gui._composer_native(window, '', 0, 0)
    service.raise_errors()
    return _refresh_return(ctx, 'notifications', evidence)


def run_commands(ctx):
    """Real slash menu, styled token, non-document ghost and origin-routed WHO."""
    from collections import Counter
    from pathlib import Path
    import math
    import sqlite3

    w, p = ctx['window'], ctx['proc']
    destination = Path(ctx['destination'])
    db = ctx['database_path']
    origin = ctx['channel_id']
    observer = ctx['observer']
    assert isinstance(observer, gui.WireObserver), type(observer)
    assert ctx['query_target'] == 'guiobserver', ctx['query_target']
    initial = gui.save_state(p, ctx['preferences'], db)
    assert initial[1] == (origin,), initial
    upload.focus_composer(w)
    empty = gui._composer_native(w, '', 0, 0)
    start = empty['caret']
    layout = gui.read_layout(ctx['preferences'])
    pane = layout['conversation']['rect']
    captures = {}

    def capture(label):
        captures[label] = str(gui.capture_window(w, destination, 'commands-' + label))
        return gui.native_rgb_capture(w)

    def pixels(image, box):
        x, y, width, height = map(round, box)
        assert 0 <= x < x + width <= image['width'], box
        assert 0 <= y < y + height <= image['height'], box
        return [tuple(image['pixels'][(row * image['width'] + col) * 3:
                                       (row * image['width'] + col) * 3 + 3])
                for row in range(y, y + height) for col in range(x, x + width)]

    def composer_band(image):
        # Use the allocated pane's blank right interior: the empty placeholder
        # extends past start+180; /msg nick and its ghost stay well to the left.
        scale = layout['scale']
        x, y = round(pane[0] + pane[2] - 200 * scale), round(start[1] + start[3] / 2)
        assert start[0] + 300 * scale < x < pane[0] + pane[2] - 100 * scale, (start, pane)
        def at(row):
            offset = (row * image['width'] + x) * 3
            return image['pixels'][offset:offset + 3]
        color = at(y)
        top, bottom = y, y + 1
        while top > pane[1] and at(top - 1) == color:
            top -= 1
        while bottom < pane[1] + pane[3] and at(bottom) == color:
            bottom += 1
        assert bottom - top >= start[3], (top, bottom, start)
        return [top, bottom, list(color)]

    baseline_band = composer_band(capture('empty'))
    stages = []

    def unchanged_height(label, state, image):
        band = composer_band(image)
        assert band == baseline_band, (label, baseline_band, band)
        stages.append({'stage': label, 'caret': state['caret'], 'composer_band': band})

    gui.type_text('/ms')
    prefix = gui.observe_native_input(w, destination, 'commands-prefix-menu', '/ms', 3, 3)
    # Description evidence is the actual painted menu, not an English wording gate.
    unchanged_height('prefix-menu', prefix, capture('prefix-menu'))
    gui.framed_keys(w, 'Tab')
    completed = gui._composer_native(w, '/msg ', 5, 5)
    unchanged_height('completion', completed, capture('completed'))
    gui.command('undo', window=w)
    undone = gui._composer_native(w, '/ms', 3, 3)
    unchanged_height('one-undo', undone, capture('one-undo'))
    # Completing again, rather than retyping /msg, keeps this a real consumer.
    gui.framed_keys(w, 'Tab')
    completed = gui._composer_native(w, '/msg ', 5, 5)
    ghost_caret = completed['caret']
    ghost_region = (ghost_caret[0], start[1] - 3, 90, start[3] + 6)
    ghost_label = gui.SESSION.until('native missing nickname glyphs', lambda:
        upload.label_rect(w, destination, 'nick', region=ghost_region))
    ghost_image = capture('ghost')
    unchanged_height('ghost', completed, ghost_image)
    # Exact compositor-facing text/byte offsets exclude the painted suffix.
    assert completed['text'] == '/msg ' and completed['cursor'] == completed['anchor'] == 5

    # Paint the identical suffix as ordinary committed text. Comparing the SAME
    # glyph positions distinguishes dim ghost ink from a length/nonempty check.
    gui.type_text('nick')
    committed = gui._composer_native(w, '/msg nick', 9, 9)
    committed_label = gui.SESSION.until('ordinary committed nickname glyphs', lambda:
        upload.label_rect(w, destination, 'nick', region=ghost_region))
    assert all(abs(a - b) <= 2 for a, b in zip(ghost_label, committed_label)), (
        ghost_label, committed_label)
    committed_image = capture('committed-reference')
    unchanged_height('committed-reference', committed, committed_image)
    gx, gy, gw, gh = committed_label
    ink_box = (gx - 2, gy - 2, gw + 4, gh + 4)
    normal_pixels = pixels(committed_image, ink_box)
    ghost_pixels = pixels(ghost_image, ink_box)
    background = Counter(normal_pixels).most_common(1)[0][0]
    distance = lambda a, b: math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))
    maximum = max(distance(color, background) for color in normal_pixels)
    assert maximum >= 30, ('Ordinary glyphs have no meaningful contrast', maximum)
    mask = [index for index, color in enumerate(normal_pixels)
            if distance(color, background) >= maximum * .8]
    assert len(mask) >= 12, ('Too little actual nickname glyph ink', len(mask))
    width = round(ink_box[2])
    assert len({index % width for index in mask}) >= 6
    assert len({index // width for index in mask}) >= 5
    normal_ink = tuple(round(sum(normal_pixels[i][c] for i in mask) / len(mask)) for c in range(3))
    ratios = [distance(ghost_pixels[i], background) / distance(normal_pixels[i], background)
              for i in mask]
    ratio = sorted(ratios)[len(ratios) // 2]
    assert .20 <= ratio <= .65, ('Ghost is not dim painted nickname ink', ratio)
    assert sum(.15 <= value <= .70 for value in ratios) >= len(mask) * .75, ratios

    # The command token must use its accent ink, unlike ordinary argument text.
    token_box = (start[0], start[1] - 3, ghost_caret[0] - start[0] - 2, start[3] + 6)
    token_pixels = pixels(committed_image, token_box)
    token_max = max(distance(color, background) for color in token_pixels)
    token_ink_pixels = [color for color in token_pixels if distance(color, background) >= token_max * .8]
    assert len(token_ink_pixels) >= 16, 'Command token has no meaningful painted glyph ink'
    token_ink = tuple(round(sum(color[c] for color in token_ink_pixels) / len(token_ink_pixels))
                      for c in range(3))
    assert distance(token_ink, normal_ink) >= 15, ('Command token is not accent styled', token_ink, normal_ink)

    gui.framed_keys(w, 'shift+Left', 'shift+Left', 'shift+Left', 'shift+Left')
    gui._composer_native(w, '/msg nick', 5, 9)
    gui.framed_keys(w, 'BackSpace')
    gui._composer_native(w, '/msg ', 5, 5)
    gui.framed_keys(w, 'ctrl+Home', 'ctrl+shift+End')
    selected = gui._composer_native(w, '/msg ', 5, 0)
    gui.seed_clipboard('private-command-copy-sentinel', ctx['cleanup'])
    gui.framed_keys(w, 'alt+w')
    gui.SESSION.until('copy excludes native nickname ghost', lambda: gui.clipboard_text() == '/msg ')
    copied = gui.clipboard_text()
    assert copied == '/msg ', copied
    gui._composer_native(w, '/msg ', 5, 0)
    capture('selection-excludes-ghost')
    gui.clear_draft()
    gui._composer_native(w, '', 0, 0)

    # Query the real daemon independently for identity data, not an expected
    # English sentence or the application's own command echo.
    wire_start = len(observer.frames)
    observer.send('WHO ' + ctx['channel'])
    identity = source = None
    while True:
        command, params, sender = observer.wait(lambda command, params, prefix:
            command in ('352', '315') and len(params) >= 2 and params[1] == ctx['channel'],
            timeout=15)
        if command == '315':
            terminal, terminal_source = params, sender
            break
        if len(params) >= 8 and params[5] == ctx['query_target']:
            identity, source = params, sender
    assert identity and all(identity[index] for index in (2, 5, 7)), identity
    hopcount, realname = identity[7].split(' ', 1)
    assert hopcount.isdigit() and realname, identity
    expected_identity = {'nick': identity[5], 'user': identity[2], 'realname': realname}
    with sqlite3.connect(f'file:{db}?mode=ro', uri=True) as database:
        frontier = database.execute('SELECT COALESCE(MAX(seq),0) FROM messages').fetchone()[0]
        origin_row = database.execute('SELECT id,server,target,kind FROM buffers WHERE id=?',
                                      (origin,)).fetchone()
        assert origin_row and origin_row[2:] == (ctx['channel'], 'channel'), origin_row
        server_rows = database.execute('SELECT id,server,target,kind FROM buffers WHERE server=? AND kind=?',
                                       (origin_row[1], 'server')).fetchall()
        assert server_rows, 'Private server buffer identity missing'
    assert gui.save_state(p, ctx['preferences'], db)[1] == (origin,)
    who_command = '/who ' + ctx['channel']
    gui.type_text(who_command)
    gui._composer_native(w, who_command, len(who_command.encode()), len(who_command.encode()))
    gui.framed_keys(w, 'Return')
    gui._composer_native(w, '', 0, 0)

    def retained_reply():
        gui.save_state(p, ctx['preferences'], db)
        with sqlite3.connect(f'file:{db}?mode=ro', uri=True) as database:
            rows = database.execute('SELECT seq,buffer,nick,text,presence FROM messages WHERE seq>? ORDER BY seq',
                                    (frontier,)).fetchall()
        identities = []
        for row in rows:
            fields = row[3].split(' ', 8)
            if (len(fields) == 9 and fields[0] == '352' and
                    fields[1] == ctx['channel'] and fields[5] == expected_identity['nick'] and
                    fields[2] == expected_identity['user'] and fields[8] == expected_identity['realname']):
                identities.append(row)
        endings = [row for row in rows if row[3].startswith('315 ' + ctx['channel'] + ' ')]
        if not identities or not endings:
            return False
        assert all(row[1] == origin and row[4] == 1 for row in identities + endings), rows
        numerics = [row for row in rows if row[3].startswith(('352 ' + ctx['channel'] + ' ',
                                                           '315 ' + ctx['channel'] + ' '))]
        assert all(row[1] == origin and row[4] == 1 for row in numerics), numerics
        assert not any(row[1] in {server[0] for server in server_rows}
                       and 'guiobserver' in row[3] for row in rows), rows
        return {'identity_rows': identities, 'terminal_rows': endings, 'numeric_rows': numerics}

    retained = gui.SESSION.until('real WHO identity and terminal numerics retained in origin only', retained_reply)
    assert gui.save_state(p, ctx['preferences'], db)[1] == (origin,)
    who_native = gui.observe_native_input(
        w, destination, 'commands-who-rendered', '', 0, 0)
    capture('who-origin')
    return _refresh_return(ctx, 'commands', [{
        'prefix': prefix['text'],
        'menu_description_evidence': {'kind': 'capture-only', 'capture': captures['prefix-menu']},
        'completed_text': completed['text'], 'one_undo_text': undone['text'],
        'height_stages': stages, 'ghost_text': 'nick', 'ghost_rect': ghost_label,
        'native_committed': completed, 'native_selection': selected, 'copied': copied,
        'ghost_ink_samples': len(mask), 'ghost_to_committed_contrast': ratio,
        'command_ink_rgb': token_ink, 'argument_ink_rgb': normal_ink,
        'origin_buffer': origin_row, 'server_buffers': server_rows, 'frontier': frontier,
        'wire_identity': {'command': '352', 'params': identity, 'prefix': source},
        'wire_terminal': {'command': '315', 'params': terminal, 'prefix': terminal_source},
        'wire_frames': observer.frames[wire_start:], 'expected_identity': expected_identity,
        'retained': retained,
        'who_render_evidence': {'kind': 'capture-only', 'capture': captures['who-origin'],
                                'native': who_native},
        'captures': captures,
    }])


def run_replies(ctx):
    """Reuse the real reply consumer without replaying unrelated composer scopes."""
    _policy_focus(ctx, ctx['query_id'])
    observer = ctx['observer']
    observer.send('CAP REQ :message-tags echo-message')
    observer.wait(lambda command, params, prefix:
        command == 'CAP' and ('ACK' in params or 'NAK' in params), timeout=15)
    assert {'message-tags', 'echo-message'} <= observer.capabilities, observer.capabilities
    rows = gui.probe_keyboard_reply(
        ctx['proc'], ctx['window'], ctx['preferences'], ctx['database_path'],
        ctx['args'], Path(ctx['destination']), observer,
        query_id=ctx['query_id'], query_target=ctx['query_target'], own_nick=ctx['own_nick'],
        cleanup=ctx['cleanup'], lab=ctx['lab'], receive=ctx['receive'])
    ctx['expected_messages'].extend(rows)
    destination = Path(ctx['destination'])
    evidence = destination.with_name(destination.stem + '-keyboard-reply-evidence.json')
    return _refresh_return(ctx, 'replies', {
        'real_reply_evidence': str(evidence), 'exact_message_rows': rows})


def run_chat_controls(ctx):
    """Real menus, durable flags, retained state and ordered IRC membership witnesses."""
    import collections
    import hashlib
    import datetime
    import re
    import sqlite3
    import time
    from pathlib import Path
    from PIL import Image

    destination = Path(ctx['destination'])
    dbpath = ctx['database_path']
    channel_id = ctx['channel_id']
    channel = ctx['channel']
    observer = ctx['observer']
    evidence = []
    serial = 0
    nonce = str(time.monotonic_ns())

    timestamp_pattern = re.compile(r'\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.(\d{1,9}))?Z')

    def server_nanoseconds(value):
        match = timestamp_pattern.fullmatch(value or '')
        if not match:
            return 0
        try:
            datetime.datetime.fromisoformat(value.replace('Z', '+00:00'))
        except ValueError:
            return 0
        return int(((match[1] or '') + '000000000')[:9])

    def rows(sql, params=()):
        with sqlite3.connect(f'file:{dbpath}?mode=ro', uri=True) as db:
            db.create_function('server_nanoseconds', 1, server_nanoseconds, deterministic=True)
            return db.execute(sql, params).fetchall()

    def flag(identity=channel_id):
        found = rows('SELECT muted,archived FROM buffers WHERE id=?', (identity,))
        assert len(found) == 1, (identity, found)
        return tuple(found[0])

    def wait_flag(expected, identity=channel_id):
        gui.SESSION.until('immediate schema9 flags without Save', lambda: flag(identity) == expected)
        return list(flag(identity))

    def saved():
        return gui.save_state(ctx['proc'], ctx['preferences'], dbpath)

    def layout():
        return gui.read_layout(ctx['preferences'])

    def snapshot():
        return {
            'buffers': rows('SELECT id,server,target,kind,draft,read_marker,unread FROM buffers ORDER BY id'),
            'history': rows('SELECT * FROM messages WHERE presence=0 ORDER BY seq'),
            'closed': rows('SELECT * FROM closed_buffers ORDER BY id'),
        }

    def assert_preserved(before):
        after = snapshot()
        assert after == before, ('Controls changed retained state', before, after)
        return {'buffers': len(after['buffers']), 'messages': len(after['history']),
                'closed_buffers': after['closed'], 'snapshot_sha256':
                hashlib.sha256(repr(after).encode()).hexdigest()}

    def region():
        return tuple(layout()['channels']['rect'])

    def row_rect(text=channel):
        identity = channel_id if text == channel else (
            ctx['query_id'] if text == ctx['query_target'] else None)
        active = rows("SELECT value FROM meta WHERE key='active_buffer'")
        if identity is not None and active == [(identity,)]:
            # Save's framed native Close is the input/paint barrier. Only the
            # exact saved current buffer may borrow its actual selected plate;
            # never infer a background conversation from a layout position.
            settings = tomllib.loads(Path(ctx['preferences']).read_text())
            palette = Path(ctx['preferences']).with_name('noctalia.toml')
            colors = (tomllib.loads(palette.read_text())['colors']
                      if palette.is_file() and settings['appearance']['theme_source']
                      not in ('builtin', 'built-in') else {})
            value = colors.get('primary_container', '#284777')
            if len(value) == 7 or (len(value) == 9 and value[7:].lower() == 'ff'):
                rgb = tuple(int(value[index:index + 2], 16) for index in (1, 3, 5))
                rectangle = region()
                x, y, width, height = rectangle
                z = ctx['args'].zoom
                plates = []
                for _ in range(2):
                    image = gui.native_rgb_capture(ctx['window'])
                    candidates = []
                    for component in gui.native_marker_components(image, rectangle, rgb):
                        left, top, right, bottom = component['bbox']
                        bw, bh = right - left, bottom - top
                        if (x < left < right < x + width and y < top < bottom < y + height
                                and width*.65 <= bw <= width
                                and max(28, 36*z) <= bh <= max(40, 52*z)
                                and component['pixels'] >= bw*bh*.65):
                            candidates.append((left, top, bw, bh))
                    if len(candidates) != 1:
                        break
                    plates.append(candidates[0])
                if (len(plates) == 2 and plates[0] == plates[1]
                        and rows("SELECT value FROM meta WHERE key='active_buffer'") == active):
                    evidence.append({'sidebar_selected_plate': list(plates[0]),
                                     'exact_active_buffer': identity,
                                     'paint_rgb': list(rgb), 'independent_captures': 2})
                    return plates[0]
        # Background rows still require the exact rendered conversation name.
        words = _policy_words(ctx, 'chat-controls-sidebar-row', rectangle=region())
        matches = [box for word, box in words if word.casefold() == text.casefold()]
        assert len(matches) <= 1, ('Ambiguous native sidebar target', text, matches)
        if matches:
            left, top, right, bottom = matches[0]
            return left, top, right - left, bottom - top

    def visible(expected, label):
        if expected:
            rect = gui.SESSION.until('actual archived/recovered conversation row', row_rect)
        else:
            # Two independent native captures avoid accepting the pre-action frame.
            gui.SESSION.until('archived row excluded even when active', lambda: row_rect() is None)
            assert row_rect() is None
            rect = None
        capture = gui.capture_window(ctx['window'], destination, 'chat-controls-' + label)
        evidence.append({'visibility': label, 'row_visible': expected, 'row_box': rect,
                         'capture': str(capture), 'flags': list(flag())})

    def filter_choice(index, compact=False):
        current = layout()
        x, y, width, height = current['channels']['rect']
        z = ctx['args'].zoom
        if compact:
            popup = gui.open_native_popup(ctx['window'],
                lambda: gui.click(ctx['window'], x + width / 2, y + 4*z + max(32, 40*z)/2))
            path = gui.capture_native_popup(ctx['window'], popup, destination,
                                            'chat-controls-compact-filter-' + str(index))
            gui.popup_keys(ctx['window'], popup, 'Home', *(['Down'] * index), 'Return')
            evidence.append({'compact_filter_index': index, 'popup_capture': str(path)})
        else:
            gui.filter_sidebar(ctx['window'], current, z, '', index)

    def open_archived(compact=False):
        filter_choice(3, compact)
        if compact:
            # Pointer-opened Select does not retain root keyboard focus. Prove
            # the only archived chat, then click its actual painted avatar.
            assert rows("SELECT id FROM buffers WHERE archived=1 AND server=? "
                        "AND kind IN ('channel','query') ORDER BY id",
                        (channel_id.split('/', 1)[0],)) == [(channel_id,)]
            current = layout()
            x, y, width, height = current['channels']['rect']
            z = ctx['args'].zoom
            header = 4*z + max(32, 40*z)
            body = (x, y + header, width, height - header)
            settings = tomllib.loads(Path(ctx['preferences']).read_text())
            palette = Path(ctx['preferences']).with_name('noctalia.toml')
            colors = (tomllib.loads(palette.read_text())['colors']
                      if palette.is_file() and settings['appearance']['theme_source']
                      not in ('builtin', 'built-in') else {})
            value = colors.get('surface_container_high', '#272A31')
            assert len(value) == 7 or (len(value) == 9 and value[7:].lower() == 'ff'), value
            rgb = tuple(int(value[index:index + 2], 16) for index in (1, 3, 5))
            avatars = []
            for _ in range(2):
                image = gui.native_rgb_capture(ctx['window'])
                candidates = []
                for component in gui.native_marker_components(image, body, rgb):
                    left, top, right, bottom = component['bbox']
                    bw, bh = right - left, bottom - top
                    if (width*.4 <= bw <= width*.8 and abs(bw-bh) <= 2*z
                            and component['pixels'] >= bw*bh*.4):
                        candidates.append((left, top, bw, bh))
                assert len(candidates) == 1, ('Ambiguous compact archived avatar', candidates)
                avatars.append(candidates[0])
            assert avatars[0] == avatars[1], avatars
            rect = avatars[0]
            capture = gui.capture_window(ctx['window'], destination, 'chat-controls-compact-recovery-target')
            evidence.append({'compact_archive_target': channel_id, 'avatar_box': list(rect),
                             'paint_rgb': list(rgb), 'independent_captures': 2,
                             'capture': str(capture)})
            gui.click(ctx['window'], rect[0] + rect[2]/2, rect[1] + rect[3]/2)
        else:
            rect = gui.SESSION.until('Archived channel is pointer-accessible', row_rect)
            gui.click(ctx['window'], rect[0] + rect[2]/2, rect[1] + rect[3]/2)
        gui.SESSION.until('opening archive keeps archive flag', lambda: flag()[1] == 1)
        saved()
        assert rows("SELECT value FROM meta WHERE key='active_buffer'") == [(channel_id,)]
        assert flag()[1] == 1

    def popup_ink(popup, label):
        nonlocal serial
        serial += 1
        rect = gui.SESSION.until('native menu action ' + label,
            lambda: upload.label_rect(ctx['window'], destination, label, region=popup['rect']))
        path = gui.capture_native_popup(ctx['window'], popup, destination,
                                       'chat-controls-icon-' + str(serial) + '-' + label.lower())
        px, py, pw, ph = popup['rect']
        scale = popup['output_scale']
        z = ctx['args'].zoom
        # Isolate the leading icon lane, excluding both text and the submenu arrow.
        left = px + 10*z
        right = min(px + 34*z, rect[0] - 4*z)
        cy = rect[1] + rect[3]/2
        box = (round((left-px)*scale), round((cy-9*z-py)*scale),
               round((right-px)*scale), round((cy+9*z-py)*scale))
        with Image.open(path) as captured:
            image = captured.convert('RGB').crop(box)
            assert image.width >= 8*scale and image.height >= 8*scale, (label, box, rect)
            background = collections.Counter(image.getdata()).most_common(1)[0][0]
            ink = [(x, y) for y in range(image.height) for x in range(image.width)
                   if max(abs(image.getpixel((x,y))[c]-background[c]) for c in range(3)) > 40]
            assert 4 <= len(ink) < image.width*image.height*.65, (label, len(ink), box)
            assert len({x for x,y in ink}) >= 3 and len({y for x,y in ink}) >= 3
            digest = hashlib.sha256(image.tobytes()).hexdigest()
        evidence.append({'surface_action': label, 'popup_capture': str(path),
                         'icon_crop_pixels': list(box), 'icon_ink_pixels': len(ink),
                         'icon_sha256': digest, 'label_box': list(rect)})
        return rect

    def action(surface, label, identity=channel_id):
        # Baseline is allowed to save; the post-action flag observation is not.
        saved()
        before = snapshot()
        active_before = rows("SELECT value FROM meta WHERE key='active_buffer'")
        old = flag(identity)
        if surface == 'sidebar':
            text = channel if identity == channel_id else ctx['query_target']
            rect = gui.SESSION.until('native sidebar context target', lambda:
                row_rect(text))
            def right_click():
                gui.move_pointer(ctx['window'], rect[0]+rect[2]/2, rect[1]+rect[3]/2)
                gui.POINTER.button(True, 0x111)
                gui.POINTER.button(False, 0x111)
            root = gui.open_native_popup(ctx['window'], right_click)
            branch = 0 if label in ('Archive', 'Unarchive') else 2
            popup = gui.open_native_submenu(ctx['window'], root, branch)
            popup_ink(popup, label)
            gui.popup_keys(ctx['window'], root, 'Home', 'Down', 'Return')
        else:
            root = gui.open_title_menu(ctx['window'], gui.title_actions(layout(), ctx['args'].zoom,
                channel=identity == channel_id))
            rect = popup_ink(root, label)
            # Root indexes vary with hidden tools: activate the observed real leaf.
            marker = gui.GUI_DEBUG[ctx['window']][1].stat().st_size
            gui.click(ctx['window'], rect[0]+rect[2]/2, rect[1]+rect[3]/2)
            gui.SESSION.until('title popup closes after leaf click', lambda:
                ('xdg_popup@' + str(root['popup']) + '.destroy(' in
                 gui.GUI_DEBUG[ctx['window']][1].read_text()[marker:] or
                 'xdg_popup#' + str(root['popup']) + '.destroy(' in
                 gui.GUI_DEBUG[ctx['window']][1].read_text()[marker:]))
            gui.await_native_frame(ctx['window'], marker,
                re.compile(r'wl_pointer[@#]\d+\.button\(\d+,\s*\d+,\s*272,\s*0\)'))
        expected = (int(label == 'Mute') if label in ('Mute', 'Unmute') else old[0],
                    int(label == 'Archive') if label in ('Archive', 'Unarchive') else old[1])
        immediate = wait_flag(expected, identity)
        if label == 'Archive' and active_before == [(identity,)]:
            assert rows("SELECT value FROM meta WHERE key='active_buffer'") == [
                (identity.split('/', 1)[0] + '/*server*',)], 'Archive failed selected-chat fallback'
        evidence.append({'surface': surface, 'action': label, 'buffer': identity,
                         'immediate_flags': immediate, 'preserved': assert_preserved(before)})

    def own(text):
        upload.focus_composer(ctx['window'])
        state = gui.native_input_state(ctx['window'])
        assert state is not None and not state['text'], state
        gui.type_text(text)
        gui.key('Return')
        ctx['receive']('PRIVMSG', [channel, text], ctx['own_nick'])
        ctx['expected_messages'].extend(ctx['channel_messages'](text, ctx['own_nick']))
        gui.wait_messages(ctx['proc'], dbpath, ctx['expected_messages'])

    def membership_witness(start, label):
        text = 'Native archive membership witness ' + nonce + ' ' + label
        own(text)
        end = next((index + 1 for index in range(start, len(observer.frames))
                    if observer.frames[index]['command'] == 'PRIVMSG' and
                    observer.frames[index]['params'] == [channel, text] and
                    observer.frames[index]['prefix'].split('!',1)[0] == ctx['own_nick']), None)
        assert end is not None, ('App connection witness not received by real observer', text)
        frames = observer.frames[start:end]
        departed = [f for f in frames if f['command'] in ('PART', 'QUIT') and
                    f['prefix'].split('!',1)[0] == ctx['own_nick'] and
                    (f['command'] == 'QUIT' or channel in f['params'][0].split(','))]
        assert not departed, departed
        evidence.append({'no_part_action_through_app_privmsg': label, 'frame_start': start,
                         'frame_end': end, 'frames': frames,
                         'witness_text': text, 'continued_channel_membership': True})

    assert rows('PRAGMA user_version') == [(9,)]
    gui.expand_navigation(ctx['proc'], ctx['window'], ctx['preferences'], dbpath)
    _policy_send(ctx, 'Native controls private query opener ' + nonce)
    _policy_focus(ctx, ctx['query_id'])
    assert not gui.native_input_state(ctx['window'])['text']
    draft_text = 'Unsent native controls draft ' + nonce
    gui.type_text(draft_text)
    gui._composer_native(ctx['window'], draft_text, len(draft_text), len(draft_text))
    _policy_focus(ctx, channel_id)
    # A newer real server-authored anchor makes startup AFTER history incapable of
    # filling the older remote-only gap. No timestamps or protocol events are forged.
    anchor_text = 'Native archive newer server anchor ' + nonce
    _policy_send(ctx, anchor_text, target=channel)
    saved()
    candidates = rows('SELECT seq,id,nick,text,time,timestamp FROM messages WHERE buffer=? '
                      "AND presence=0 AND own=0 AND pending=0 AND id<>'' "
                      "AND id NOT LIKE 'local:%' AND id NOT LIKE 'local-%' "
                      "ORDER BY sort_time,server_nanoseconds(time),seq", (channel_id,))
    assert len(candidates) >= 2, candidates
    gap = candidates[0]
    assert gap[3] != anchor_text and gap[4] and gap[5] > 0, gap
    anchor = rows('SELECT id,time FROM messages WHERE buffer=? AND text=?', (channel_id, anchor_text))
    assert len(anchor) == 1 and anchor[0][0] and anchor[0][1], anchor
    out, err = gui.finish_gui(ctx['proc'], ctx['window'], ctx['screenshot'],
        destination.with_name('chat-controls-fixture-stop.bmp'), ctx['args'].size)
    ctx['logs'].append(('chat-controls-fixture-stop', out, err))
    departed = {ctx['own_nick'], ctx['other_nick']}
    while departed:
        _, _, prefix = observer.wait(lambda command, params, prefix:
            command == 'QUIT' and prefix.split('!',1)[0] in departed, timeout=15)
        departed.remove(prefix.split('!',1)[0])
    assert ctx['proc'].poll() is not None
    with sqlite3.connect(dbpath) as db:
        # Expression-index DELETE needs the production timestamp fraction on this
        # connection. This is only the stopped, generated private fixture DB.
        db.execute('PRAGMA trusted_schema=ON')
        db.create_function('server_nanoseconds', 1, server_nanoseconds, deterministic=True)
        for value, in db.execute('SELECT time FROM messages WHERE buffer=? AND (presence<>0 OR seq=?)',
                                 (channel_id,gap[0])):
            assert not value or timestamp_pattern.fullmatch(value), ('Unsupported fixture timestamp',value)
            if value:
                datetime.datetime.fromisoformat(value.replace('Z', '+00:00'))
        # Presence rows are fixture scaffolding, not retained chat history. Remove
        # them only in Local so the first history cursor is after the remote gap.
        presence_removed = db.execute('DELETE FROM messages WHERE buffer=? AND presence<>0',
                                      (channel_id,)).rowcount
        assert db.execute('DELETE FROM messages WHERE seq=? AND buffer=?', (gap[0],channel_id)).rowcount == 1
    removed_expected = (channel_id, gap[2], gap[3], 0, 0)
    assert removed_expected in ctx['expected_messages'], (removed_expected, ctx['expected_messages'])
    ctx['expected_messages'].remove(removed_expected)
    screenshot = Path(ctx['screenshot']).with_name('chat-controls-restart.bmp')
    proc, window = gui.launch_gui(ctx['binary'], screenshot, ctx['env'], ctx['args'].size, ctx['logs'])
    ctx.update(proc=proc, window=window, screenshot=screenshot)
    gui.wait_joins(observer, channel, (ctx['own_nick'],ctx['other_nick']))
    _policy_focus(ctx, channel_id)
    gui.expand_navigation(proc, window, ctx['preferences'], dbpath)
    saved()
    assert not rows('SELECT 1 FROM messages WHERE buffer=? AND id=?', (channel_id,gap[1]))
    baseline = snapshot()
    evidence.append({'fixture_gap': {'buffer': channel_id, 'id': gap[1], 'nick': gap[2],
        'text': gap[3], 'server_time': gap[4], 'timestamp': gap[5]},
        'newer_wire_anchor': anchor, 'expected_context_removed': list(removed_expected),
        'local_presence_scaffolding_removed': presence_removed,
        'baseline_history_rows_after_fixture_prep': len(baseline['history'])})

    # Sidebar and overflow each expose all four explicit set actions with native ink.
    filter_choice(0)
    action('sidebar', 'Mute')
    start = len(observer.frames)
    action('sidebar', 'Archive')
    visible(False, 'sidebar-archive-normal')
    open_archived()
    visible(True, 'sidebar-archive-recovery')
    membership_witness(start, 'sidebar-archive')
    assert flag() == (1,1), 'Own send resurrected muted archive'
    action('sidebar', 'Unmute')
    assert flag() == (0,1), 'Unmuting alone resurrected archive'
    action('sidebar', 'Unarchive')
    filter_choice(0)
    visible(True, 'sidebar-unarchive')
    _policy_focus(ctx, channel_id)
    action('title', 'Mute')
    start = len(observer.frames)
    action('title', 'Archive')
    visible(False, 'title-archive-normal')
    open_archived()
    membership_witness(start, 'title-archive')
    action('title', 'Unmute')
    assert flag() == (0,1)
    # Load older through the actual archived row menu. The absent row must return
    # byte-for-byte with the original server id/time; startup AFTER cannot do this.
    rect = gui.SESSION.until('archived history context target', row_rect)
    def history_menu():
        gui.move_pointer(window, rect[0]+rect[2]/2, rect[1]+rect[3]/2)
        gui.POINTER.button(True, 0x111)
        gui.POINTER.button(False, 0x111)
    root = gui.open_native_popup(window, history_menu)
    gui.select_native_menu(window, root, [1,0])
    ctx['expected_messages'].append(removed_expected)
    gui.wait_messages(proc, dbpath, ctx['expected_messages'])
    hydrated = rows('SELECT id,nick,text,time,timestamp FROM messages WHERE buffer=? AND id=?',
                    (channel_id,gap[1]))
    assert hydrated == [tuple(gap[1:])], (gap, hydrated)
    assert flag() == (0,1), 'Actual CHATHISTORY hydration resurrected archive'
    evidence.append({'actual_remote_history_hydrated': hydrated, 'archived_after_history': True})
    membership_witness(len(observer.frames), 'unmuted-own-send')
    assert flag() == (0,1), 'Own send resurrected unmuted archive'
    action('title', 'Unarchive')
    filter_choice(0)
    visible(True, 'title-unarchive')

    # Muted archives retain live incoming messages AND unread while backgrounded.
    action('sidebar', 'Mute')
    muted_archive_start = len(observer.frames)
    action('sidebar', 'Archive')
    before_unread = rows('SELECT unread,read_marker,draft FROM buffers WHERE id=?', (channel_id,))[0]
    muted_text = 'Native muted archived incoming ' + nonce
    _policy_send(ctx, muted_text, target=channel)
    saved()
    after_unread = rows('SELECT unread,read_marker,draft FROM buffers WHERE id=?', (channel_id,))[0]
    assert flag() == (1,1) and after_unread[0] > before_unread[0]
    assert after_unread[1:] == before_unread[1:], (before_unread, after_unread)
    archived_total = rows("SELECT COALESCE(SUM(unread),0) FROM buffers WHERE archived=1 "
                          "AND server='Local' AND kind IN ('channel','query')")[0][0]
    controls = gui.sidebar_controls(layout(), ctx['args'].zoom)
    gui.click(window, *controls['query'])
    count_popup = gui.sidebar_filter_popup(window)
    # sidebarArchiveControlsCheck owns exact native caption/fit regression.
    # This live consumer supplies the wire-derived count and fresh UI evidence,
    # without making tiny-font OCR an additional correctness requirement.
    assert archived_total >= after_unread[0] > 0
    count_capture = (gui.capture_native_popup(window, count_popup, destination,
                                             'chat-controls-archived-count')
                     if count_popup is not None else
                     gui.capture_window(window, destination, 'chat-controls-archived-count'))
    evidence.append({'native_archived_unread_total': archived_total,
                     'filter_surface': 'selector-popup' if count_popup is not None else 'tabs',
                     'capture': str(count_capture)})
    if count_popup is not None:
        gui.popup_keys(window, count_popup, 'Escape')
    filter_choice(3)
    visible(True, 'muted-archived-unread-recovery')
    action('sidebar', 'Unmute')
    assert flag() == (0,1)
    assert rows('SELECT unread FROM buffers WHERE id=?',(channel_id,)) == [(after_unread[0],)]
    evidence.append({'muted_live_unread_before': list(before_unread),
                     'muted_live_unread_after': list(after_unread),
                     'unmute_alone_preserves_archive_and_unread': True})

    # Recover using the actual compact fourth option too, including active-row
    # exclusion in All. Opening the archive is intentionally not a restore action.
    gui.set_navigation(proc, window, ctx['preferences'], dbpath, 'compact', 'chat-controls-compact')
    filter_choice(0, compact=True)
    compact_all = gui.capture_window(window, destination, 'chat-controls-compact-all')
    open_archived(compact=True)
    membership_witness(muted_archive_start, 'muted-live-archive')
    compact_archived = gui.capture_window(window, destination, 'chat-controls-compact-archived')
    filter_choice(0, compact=True)
    assert flag() == (0,1)
    gui.expand_navigation(proc, window, ctx['preferences'], dbpath)
    visible(False, 'active-archive-excluded-after-compact-recovery')
    # Leave the target genuinely backgrounded before the eligible live arrival.
    gui.switch_buffer('*server*', 'Local')
    saved()
    live_text = 'Native unmuted archive live restoration ' + nonce
    _policy_send(ctx, live_text, target=channel)
    wait_flag((0,0))
    saved()
    visible(True, 'fresh-live-restores-unmuted')
    evidence.append({'compact_all_capture': str(compact_all),
                     'compact_archived_capture': str(compact_archived),
                     'fresh_live_restore_text': live_text, 'final_flags': list(flag())})

    # Query chats are eligible too; keep a real nonempty unsent draft throughout.
    gui.switch_buffer(ctx['query_target'], 'Local')
    saved()
    assert rows('SELECT draft FROM buffers WHERE id=?',(ctx['query_id'],)) == [(draft_text,)]
    action('sidebar', 'Mute', ctx['query_id'])
    action('sidebar', 'Archive', ctx['query_id'])
    filter_choice(3)
    query_rect = gui.SESSION.until('archived query recovery', lambda:
        row_rect(ctx['query_target']))
    gui.click(window, query_rect[0]+query_rect[2]/2, query_rect[1]+query_rect[3]/2)
    saved()
    assert rows("SELECT value FROM meta WHERE key='active_buffer'") == [(ctx['query_id'],)]
    assert flag(ctx['query_id']) == (1,1)
    action('title', 'Unmute', ctx['query_id'])
    action('title', 'Unarchive', ctx['query_id'])
    filter_choice(0)
    assert flag(ctx['query_id']) == (0,0)
    assert rows('SELECT draft FROM buffers WHERE id=?',(ctx['query_id'],)) == [(draft_text,)]
    gui.switch_buffer('*server*', 'Local')
    saved()
    server_before = snapshot()
    root = gui.open_title_menu(window, gui.title_actions(layout(), ctx['args'].zoom, channel=False))
    server_words = upload.visible_words(window, destination, 'chat-controls-server-ineligible',
                                         region=root['rect'])
    assert not {'mute','unmute','archive','unarchive'} & {
        word['text'].casefold().strip('.,') for word in server_words}, server_words
    server_capture = gui.capture_native_popup(window, root, destination, 'chat-controls-server-ineligible')
    gui.popup_keys(window, root, 'Escape')
    for command in ('mute-buffer', 'archive-buffer'):
        gui.command(command, window=window)
        saved()
        assert rows("SELECT muted,archived FROM buffers WHERE id='Local/*server*'") == [(0,0)]
    assert_preserved(server_before)
    evidence.append({'query_draft_preserved': draft_text, 'query_final_flags': list(flag(ctx['query_id'])),
                     'server_controls_absent_capture': str(server_capture),
                     'server_explicit_commands_rejected': True})

    # Original user history survives every control; only the documented real
    # history gap acquired a new SQLite sequence on hydration.
    final_history = rows('SELECT * FROM messages WHERE presence=0 ORDER BY seq')
    by_seq = {row[0]: row for row in final_history}
    assert all(by_seq.get(row[0]) == row for row in baseline['history'])
    assert rows('SELECT * FROM closed_buffers ORDER BY id') == baseline['closed']
    assert rows('SELECT id,server,target,kind,draft FROM buffers ORDER BY id') == [
        row[:5] for row in baseline['buffers']]
    return _refresh_return(ctx, 'chat-controls', evidence)


def run_irc_polish(ctx):
    """Real IRC rows; only retained timestamps are synthetic, never server tags."""
    import collections
    import datetime
    import json

    assert ctx['args'].palette is not None, 'irc-polish requires an owned generated --palette'
    palette_path = Path(ctx['preferences']).parent / 'noctalia.toml'
    colors = tomllib.loads(palette_path.read_text())['colors']
    def color(key):
        value = colors[key]
        assert re.fullmatch(r'#[0-9a-fA-F]{6}(?:[0-9a-fA-F]{2})?', value), (key, value)
        return tuple(int(value[i:i + 2], 16) for i in (1, 3, 5))
    primary = color('primary')
    def luminance(rgb):
        channels = [v / 255 for v in rgb]
        channels = [v / 12.92 if v <= .04045 else ((v + .055) / 1.055) ** 2.4 for v in channels]
        return sum(a * b for a, b in zip(channels, (.2126, .7152, .0722)))
    def contrast(a, b):
        a, b = luminance(a), luminance(b)
        return (max(a, b) + .05) / (min(a, b) + .05)
    # The fixture deliberately requires an already-accessible palette, avoiding
    # a second implementation of the theme's authored-alpha repair policy.
    foreground = color('on_primary')
    assert len(colors['on_primary']) == 7 and contrast(primary, foreground) >= 4.5, colors
    assert primary != foreground
    ink_vector = tuple(fg - bg for bg, fg in zip(primary, foreground))
    ink_norm = sum(channel * channel for channel in ink_vector)
    def on_primary_ink(rgb):
        # Tabler's 12px antialiased strokes need not reach the solid foreground.
        # Require majority coverage of the authored ink, not any changed pixel.
        coverage = sum((v - bg) * delta for v, bg, delta in zip(rgb, primary, ink_vector)) / ink_norm
        return .5 <= coverage <= 1.05 and max(
            abs(v - (bg + coverage * delta)) for v, bg, delta in zip(rgb, primary, ink_vector)) <= 12
    _policy_focus(ctx, ctx['channel_id'])
    gui.set_navigation(ctx['proc'], ctx['window'], ctx['preferences'], ctx['database_path'],
                       'hidden', 'irc-polish-wide')
    gui.set_people(ctx['proc'], ctx['window'], ctx['preferences'], ctx['database_path'], 'hidden')
    _policy_typing_cap(ctx)  # Real echo-message makes observer-origin PRIVMSG observable too.
    _policy_effects(ctx, fx='off')
    _policy_chat(ctx, fools=[])
    gui.set_message_style(ctx['proc'], ctx['window'], ctx['preferences'], ctx['database_path'],
        ctx['args'], ctx['destination'], ctx['channel_id'], 'irc', label='irc-polish-style')
    assert tomllib.loads(Path(ctx['preferences']).read_text())['appearance']['message_wrap_columns'] == 80
    scale = gui.read_layout(ctx['preferences'])['scale']
    assert scale > 0
    marker_colors = [(42, 185, 113), (179, 79, 213), (211, 121, 37), (37, 151, 211)]
    texts, wire = [], []
    token = str(time.monotonic_ns())
    for index, rgb in enumerate(marker_colors):
        hex_color = ''.join(f'{v:02X}' for v in rgb)
        text = '\x04' + hex_color + ',' + hex_color + 'SOLIDMARKER\x0f ircpolish-' + token + '-' + str(index)
        texts.append(text)
        if index % 2 == 0:
            _policy_send(ctx, text, target=ctx['channel'])
            nick = ctx['query_target']
        else:
            nick = ctx['own_nick']
            upload.focus_composer(ctx['window'])
            _refresh_clipboard(ctx, text.encode('utf-8'), 'text/plain;charset=utf-8')
            gui.framed_keys(ctx['window'], 'ctrl+v')
            gui._composer_native(ctx['window'], text, len(text.encode('utf-8')), len(text.encode('utf-8')))
            gui.framed_keys(ctx['window'], 'Return')
            ctx['expected_messages'].extend(ctx['channel_messages'](text, nick))
        frame = ctx['receive']('PRIVMSG', [ctx['channel'], text], nick)
        wire.append({'nick': nick, 'target': ctx['channel'], 'text': text, 'observed': repr(frame)})
        gui.wait_messages(ctx['proc'], ctx['database_path'], ctx['expected_messages'])
    # Tomorrow and the following day keep these four rows together after the
    # existing fixture without rewriting its history. Local civil times are
    # converted to UTC retained time strings; sort_time is seconds, not ns.
    day = datetime.date.today() + datetime.timedelta(days=1)
    civil = [(day, 12, 34, 1), (day, 12, 34, 49), (day, 12, 35, 1),
             (day + datetime.timedelta(days=1), 12, 35, 30)]
    timestamps = [int(time.mktime((d.year, d.month, d.day, h, m, s, 0, 0, -1)))
                  for d, h, m, s in civil]
    retained = []
    def server_nanoseconds(value):
        match = re.fullmatch(r'([0-9]{4})-([0-9]{2})-([0-9]{2})[Tt]([0-9]{2}):([0-9]{2}):([0-9]{2})(?:\.([0-9]+))?([Zz]|[+-][0-9]{2}:[0-9]{2})', value or '')
        if not match:
            return 0
        try:
            datetime.datetime(*(int(part) for part in match.groups()[:6]))
        except ValueError:
            return 0
        zone = match[8]
        if len(zone) > 1 and (int(zone[1:3]) > 23 or int(zone[4:6]) > 59):
            return 0
        return int(((match[7] or '') + '000000000')[:9])
    def restart(label, avatars, timestamps_on, seed=False):
        gui.save_state(ctx['proc'], ctx['preferences'], ctx['database_path'])
        target = Path(ctx['destination']).with_name('irc-polish-before-' + label + '.bmp')
        out, err = gui.finish_gui(ctx['proc'], ctx['window'], ctx['screenshot'], target, ctx['args'].size)
        ctx['logs'].append(('irc-polish-before-' + label, out, err))
        departed = {ctx['own_nick'], ctx['other_nick']}
        while departed:
            _, _, prefix = ctx['observer'].wait(lambda command, params, prefix:
                command == 'QUIT' and prefix.split('!', 1)[0] in departed, timeout=15)
            departed.remove(prefix.split('!', 1)[0])
        assert ctx['proc'].poll() == 0, 'Never edit a live database'
        if seed:
            with sqlite3.connect(ctx['database_path']) as database:
                database.create_function('server_nanoseconds', 1, server_nanoseconds, deterministic=True)
                for text, stamp in zip(texts, timestamps):
                    rows = database.execute('SELECT seq,buffer,nick,text,own,pending FROM messages WHERE text=?', (text,)).fetchall()
                    assert len(rows) == 2 and {row[1] for row in rows} == {ctx['channel_id'], ctx['other_channel_id']}, rows
                    iso = datetime.datetime.fromtimestamp(stamp, datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%S.000000000Z')
                    database.execute('UPDATE messages SET time=?,timestamp=?,sort_time=? WHERE text=?', (iso, stamp, stamp, text))
                    assert database.execute('SELECT seq,buffer,nick,text,own,pending FROM messages WHERE text=?', (text,)).fetchall() == rows
                    retained.append({'rows': rows, 'time': iso, 'timestamp': stamp, 'sort_time': stamp})
        preferences = Path(ctx['preferences'])
        original = preferences.read_text()
        appearance = tomllib.loads(original)['appearance']
        appearance.update(avatars=avatars, avatar_style='monogram', timestamps=timestamps_on)
        block = '[appearance]\n' + ''.join(key + ' = ' + json.dumps(value, ensure_ascii=False) + '\n'
                                             for key, value in appearance.items())
        replaced, count = re.subn(r'(?ms)^\[appearance\]\n.*?(?=^\[|\Z)', lambda match: block + '\n', original)
        assert count == 1
        preferences.write_text(replaced)
        preferences.chmod(0o600)
        screenshot = Path(ctx['screenshot']).with_name('irc-polish-' + label + '.bmp')
        proc, window = gui.launch_gui(ctx['binary'], screenshot, ctx['env'], ctx['args'].size, ctx['logs'])
        ctx.update(proc=proc, window=window, screenshot=screenshot)
        gui.wait_joins(ctx['observer'], ctx['channel'], (ctx['own_nick'], ctx['other_nick']))
        _policy_focus(ctx, ctx['channel_id'])
        upload.focus_composer(window)
        gui.move_pointer(window, 0, 0)
        boxes = [_policy_wait_box(ctx, rgb) for rgb in marker_colors]
        image = gui.native_rgb_capture(window)
        # Re-find every swatch in the same capture, not four different frames.
        rect = gui.read_layout(ctx['preferences'])['conversation']['rect']
        boxes = []
        for rgb in marker_colors:
            components = gui.native_marker_components(image, rect, rgb)
            assert len(components) == 1 and components[0]['full'], components
            boxes.append(components[0]['bbox'])
        assert all(boxes[i][3] < boxes[i + 1][1] for i in range(3)), boxes
        assert max(box[0] for box in boxes) - min(box[0] for box in boxes) <= 1, ('Need wide inline IRC pane', boxes)
        path = Path(ctx['destination']).with_name('irc-polish-' + label + '.png')
        Image.frombytes('RGB', (image['width'], image['height']), image['pixels']).save(path)
        return image, boxes, str(path)
    off, off_boxes, off_path = restart('avatars-off-clock-off', False, False, seed=True)
    on, on_boxes, on_path = restart('avatars-on-clock-off', True, False)
    def pixel(image, x, y):
        x, y = int(x), int(y)
        assert 0 <= x < image['width'] and 0 <= y < image['height'], (x, y)
        offset = (y * image['width'] + x) * 3
        return tuple(image['pixels'][offset:offset + 3])
    avatar_evidence = []
    # Align each pair by its own solid BODY marker. Day chips, presence notices,
    # unread dividers and avatar-induced row-height changes cannot define a band.
    def band_offsets(all_boxes, index):
        centers = [(box[1] + box[3]) // 2 for box in all_boxes]
        center = centers[index]
        low, high = center - round(18 * scale), center + round(18 * scale) + 1
        if index:
            low = max(low, (centers[index - 1] + center + 1) // 2)
        if index + 1 < len(centers):
            high = min(high, (center + centers[index + 1] + 1) // 2)
        return low - center, high - center
    for index, (a, b) in enumerate(zip(off_boxes, on_boxes)):
        ay, by = (a[1] + a[3]) // 2, (b[1] + b[3]) // 2
        points = []
        surface_left = gui.read_layout(ctx['preferences'])['conversation']['rect'][0] + 8 * scale
        pane = gui.read_layout(ctx['preferences'])['conversation']['rect']
        blank_left = round(pane[0] + pane[2] - 64 * scale)
        blank_right = round(pane[0] + pane[2] - 32 * scale)
        a_low, a_high = band_offsets(off_boxes, index)
        b_low, b_high = band_offsets(on_boxes, index)
        for dy in range(max(a_low, b_low), min(a_high, b_high)):
            blank = {pixel(off, x, ay + dy) for x in range(blank_left, blank_right)}
            assert len(blank) == 1, ('Need a genuinely empty off-row background sample', index, dy, blank)
            background = next(iter(blank))
            for dx in range(-round(220 * scale), -round(8 * scale)):
                if b[0] + dx < surface_left:
                    continue
                # The new 26px lane has no body-registered off counterpart.
                # Compare it to observed blank pixels in THIS off row, never
                # to the pane border and never discard the avatar itself.
                reference = (pixel(off, a[0] + dx, ay + dy)
                             if a[0] + dx >= surface_left else background)
                if pixel(on, b[0] + dx, by + dy) != reference:
                    points.append((b[0] + dx, by + dy))
        assert points, ('Avatar did not change actual pixels', index)
        bounds = (min(x for x, y in points), min(y for x, y in points),
                  max(x for x, y in points) + 1, max(y for x, y in points) + 1)
        assert abs(bounds[2] - bounds[0] - 20 * scale) <= 2 and abs(bounds[3] - bounds[1] - 20 * scale) <= 2, ('Avatar must be 20px square', index, bounds, scale)
        avatar_evidence.append({'row': index, 'bbox': bounds, 'changed_pixels': len(points)})
    final, boxes, final_path = restart('avatars-on-clock-on', True, True)
    # Persist the exact checked capture, not a window handle from before restart.
    final_surface = Image.frombytes('RGB', (final['width'], final['height']), final['pixels'])
    final_path = str(Path(ctx['destination']).with_name(Path(ctx['destination']).stem + '-irc-polish-final.png'))
    final_surface.save(final_path)
    rect = gui.read_layout(ctx['preferences'])['conversation']['rect']
    clock_left = round(rect[0] + 8 * scale)
    clock_right = clock_left + round(40 * scale)
    clock_evidence, own_evidence = [], []
    for index, box in enumerate(boxes):
        center = (box[1] + box[3]) // 2
        top, bottom = center - round(10 * scale), center + round(10 * scale)
        clock_pixels = [pixel(final, x, y) for y in range(top, bottom)
                        for x in range(clock_left, clock_right)]
        background, count = collections.Counter(clock_pixels).most_common(1)[0]
        ink = len(clock_pixels) - count
        expected_visible = index != 1
        assert (ink >= 8 * scale) if expected_visible else (ink == 0), ('Formatted clock gutter', index, ink)
        clock_evidence.append({'row': index, 'body_marker': box, 'gutter': [clock_left, top, clock_right, bottom],
                               'background_rgb': background, 'ink_pixels': ink, 'visible': expected_visible})
        if index % 2 == 0:
            continue
        # Find the actual connected primary-fill nick chip, not an OCR word or
        # presumed sender width.
        components = gui.native_marker_components(final, (clock_right, top - 8 * scale,
            box[0] - clock_right - 4 * scale, bottom - top + 16 * scale), primary)
        chips = [item for item in components if item['bbox'][2] - item['bbox'][0] > 30 * scale
                 and item['bbox'][3] - item['bbox'][1] >= 18 * scale]
        assert len(chips) == 1, ('Missing unique own accent chip', components)
        left, ct, right, cb = chips[0]['bbox']
        assert pixel(final, left, ct) != primary and pixel(final, (left + right) // 2, ct) == primary, 'Own chip must be rounded'
        name_ink = sum(on_primary_ink(pixel(final, x, y))
                       for y in range(ct + 2, cb - 2) for x in range(left + round(6 * scale), right - round(6 * scale)))
        assert name_ink >= 8 * scale, 'Own nickname lacks contrasting on-primary ink'
        # Former own stripe occupied exactly the first 2px of the dense surface.
        stripe = [pixel(final, x, y) for y in range(top, bottom)
                  for x in range(clock_left, clock_left + max(1, round(2 * scale)))]
        assert primary not in stripe, ('Obsolete IRC own left stripe remains', index)
        avatar = avatar_evidence[index]['bbox']
        # Registered clock-off avatar bounds translate with body marker when the
        # time gutter is added; no guessed row origin or separator dimensions.
        expected_avatar_right = avatar[2] + box[0] - on_boxes[index][0]
        assert abs(left - expected_avatar_right - 6 * scale) <= 2, ('Avatar-to-chip gap must be 6px', left, expected_avatar_right)
        own_evidence.append({'row': index, 'chip': [left, ct, right, cb], 'primary_rgb': primary,
            'on_primary_rgb': foreground, 'contrast': contrast(primary, foreground),
            'nickname_ink_pixels': name_ink, 'old_left_stripe_absent': True})
    with sqlite3.connect(f'file:{ctx["database_path"]}?mode=ro', uri=True) as database:
        for text, stamp in zip(texts, timestamps):
            actual = database.execute('SELECT time,timestamp,sort_time FROM messages WHERE buffer=? AND text=?',
                                      (ctx['channel_id'], text)).fetchone()
            assert actual and actual[1:] == (stamp, stamp), actual
    gui.wait_messages(ctx['proc'], ctx['database_path'], ctx['expected_messages'])
    return {'native_consumer': True, 'private_fixture_only': True, 'wire': wire,
        'timestamp_setup': 'Synthetic retained time/timestamp/sort_time on selected real-wire rows, edited only while owned app stopped; no server-forged tags',
        'retained_timestamp_fixture': retained, 'formatted_local_clocks': [time.strftime('%Y-%m-%d %H:%M', time.localtime(t)) for t in timestamps],
        'clock_gutters': clock_evidence, 'avatars': avatar_evidence, 'own_markers': own_evidence,
        'captures': [off_path, on_path, final_path], 'scale': scale,
        'wrap_copy_coverage': 'Separate existing transcript scope: default80, both native styles, real wrap and exact copy; not duplicated',
        'retained_expected_messages': list(ctx['expected_messages'])}


def run_spelling(ctx):
    """Real private composer: dictionary raster, pointer correction and discrete Undo."""
    import csv
    import io
    import sqlite3

    destination = Path(ctx['destination'])
    evidence = []
    captures = {}
    observer = ctx['observer']
    wire_start = len(observer.frames)
    preferences = Path(ctx['preferences'])
    gui.save_state(ctx['proc'], preferences, ctx['database_path'])
    initial = tomllib.loads(preferences.read_text())['composer']
    assert initial['spellcheck'] is True and initial['spellcheck_language'] == 'auto', initial
    assert ctx['args'].palette is not None, 'spelling requires --palette with an opaque Error color'
    palette_path = preferences.with_name('noctalia.toml')
    palette = tomllib.loads(palette_path.read_text())
    authored_error = palette['colors']['error']
    assert len(authored_error) == 7 or (len(authored_error) == 9 and authored_error[7:].lower() == 'ff'), authored_error
    error_rgb = tuple(int(authored_error[index:index + 2], 16) for index in (1, 3, 5))
    gui._composer_native(ctx['window'], '', 0, 0)

    def save_rgb(label, image):
        path = destination.with_name(destination.stem + '-spelling-' + label + '.png')
        Image.frombytes('RGB', (image['width'], image['height']), image['pixels']).save(path)
        captures[label] = str(path)
        return path

    def glyph_words(path, label, output_scale):
        # Observe genuine glyphs without dictionary/word-list hints or fuzzy matching.
        with Image.open(path) as source:
            source = source.convert('RGB')
            enlarged = source.resize((source.width * 4, source.height * 4), Image.Resampling.LANCZOS)
            mask = destination.with_name(destination.stem + '-spelling-' + label + '-ocr.png')
            enlarged.save(mask)
        result = subprocess.run(['tesseract', str(mask), 'stdout', '--psm', '6',
            '-c', 'load_system_dawg=0', '-c', 'load_freq_dawg=0', 'tsv'],
            check=True, capture_output=True, text=True, timeout=20)
        words = []
        for row in csv.DictReader(io.StringIO(result.stdout), delimiter='\t'):
            text = row.get('text', '').strip()
            if text:
                words.append({'text': text, 'box': [int(row[key]) / (4 * output_scale)
                    for key in ('left', 'top', 'width', 'height')]})
        return words, str(mask)

    def popup_click(popup, box):
        x, y, width, height = box
        gx = popup['global_rect'][0] + x + width / 2
        gy = popup['global_rect'][1] + y + height / 2
        output = popup['output_rect']
        assert output['x'] <= gx < output['x'] + output['width'], (gx, popup)
        assert output['y'] <= gy < output['y'] + output['height'], (gy, popup)
        gui.POINTER.move(gx, gy, *gui.display_geometry())
        gui.POINTER.button(True, code=272)
        gui.POINTER.button(False, code=272)

    def error_decoration(old, actual):
        delta = [a - b for a, b in zip(actual, old)]
        direction = [a - b for a, b in zip(error_rgb, old)]
        length2 = sum(value * value for value in delta)
        norm2 = sum(value * value for value in direction)
        if length2 < 8 ** 2 or not norm2:
            return False
        coverage = sum(a * b for a, b in zip(delta, direction)) / norm2
        residual2 = sum((a - coverage * b) ** 2 for a, b in zip(delta, direction))
        # Antialiased decoration composites authored Error over THIS baseline.
        return 0 < coverage <= 1.05 and residual2 <= length2 * .2 ** 2

    # An unavailable language is not selectable from installed dictionaries. Only
    # this disposable fixture's config is changed; restart replaces ctx handles.
    _refresh_restart(ctx, 'composer', {'spellcheck': 'true', 'spellcheck_language': '"zz_ZZ"'})
    w = ctx['window']
    gui.open_settings(w, preferences)
    gui.framed_keys(w, 'shift+Tab')
    gui.settings_category(w, ctx['args'].zoom, 0)
    # Category navigation owns focus; nine theme/typography controls precede these.
    gui.framed_keys(w, *(['Tab'] * 10))
    saved_unavailable = tomllib.loads(preferences.read_text())
    assert saved_unavailable['composer'] == {'spellcheck': True, 'spellcheck_language': 'zz_ZZ'}
    dialog = tuple(float(value) for value in saved_unavailable['workspace']['native_dialog_rect'].split())
    status_words = []
    status_raster = {}
    available_status_raster = {}
    def unavailable_status(require_error=True):
        nonlocal status_words
        status_words = upload.visible_words(w, destination, 'spelling-unavailable-locator', region=dialog)
        image = gui.native_rgb_capture(w)
        if 'unavailable-settings-first' not in captures:
            save_rgb('unavailable-settings-first', image)
        labels = {}
        for name in ('Composer spelling', 'Check spelling'):
            wanted = name.casefold().split()
            for first, second in zip(status_words, status_words[1:]):
                if [first['text'].casefold(), second['text'].casefold()] == wanted:
                    left = min(int(word['left']) for word in (first, second))
                    top = min(int(word['top']) for word in (first, second))
                    right = max(int(word['left']) + int(word['width']) for word in (first, second))
                    bottom = max(int(word['top']) + int(word['height']) for word in (first, second))
                    labels[name] = (left, top, right, bottom)
        if len(labels) != 2:
            return None
        heading, toggle = labels['Composer spelling'], labels['Check spelling']
        line_height = toggle[3] - toggle[1]
        # Only the actual section's status above the focused toggle, never screen-wide red.
        left, top = toggle[0], heading[3] + 1
        right = min(image['width'], int(dialog[0] + dialog[2] - 18 * ctx['args'].zoom))
        bottom = toggle[1] - 1
        if not (0 <= left < right and 0 <= top < bottom <= image['height'] and
                bottom - top <= 8 * line_height):
            return None
        region = Image.frombytes('RGB', (image['width'], image['height']), image['pixels']).crop(
            (left, top, right, bottom))
        background = max(region.getcolors(region.width * region.height), key=lambda item: item[0])[1]
        ink = []
        mask = Image.new('RGB', region.size, 'black')
        for yy in range(region.height):
            for xx in range(region.width):
                actual = region.getpixel((xx, yy))
                if error_decoration(background, actual):
                    ink.append((xx, yy))
                    mask.putpixel((xx, yy), actual)
        mask_path = destination.with_name(destination.stem + '-spelling-unavailable-status-mask.png')
        # Keep the first status raster and mask even when the following evidence gate fails.
        if 'unavailable-status-mask' not in captures:
            mask.save(mask_path)
            captures['unavailable-status-mask'] = str(mask_path)
        if not require_error:
            if ink:
                return None
            available_status_raster.update(region=(left, top, right, bottom),
                                          error_ink_pixels=0, control_labels=labels)
            save_rgb('available-settings', image)
            return (left, top, right - left, bottom - top)
        xs, ys = {point[0] for point in ink}, {point[1] for point in ink}
        if not (len(xs) >= 6 * line_height and len(ys) >= max(3, line_height // 2) and
                len(ink) >= 8 * line_height):
            return None
        bounds = (min(xs), min(ys), max(xs) + 1, max(ys) + 1)
        if len(ink) >= (bounds[2] - bounds[0]) * (bounds[3] - bounds[1]) * .5:
            return None  # Text-shaped coverage, not a solid Error-colored background.
        status_raster.update(region=(left, top, right, bottom), background_rgb=background,
                             ink_pixels=len(ink), ink_columns=len(xs), ink_rows=len(ys),
                             ink_bounds=bounds, control_labels=labels)
        save_rgb('unavailable-settings', image)
        return (left + bounds[0], top + bounds[1], bounds[2] - bounds[0], bounds[3] - bounds[1])
    unavailable = gui.SESSION.until('saved unavailable language and visible section Error glyphs', unavailable_status)
    unavailable_words = list(status_words)
    # Native Tab focus requests scrollIntoView for the actual following Select.
    gui.framed_keys(w, 'Tab')
    language_popup = gui.open_native_popup(w, lambda: gui.key('Return'))
    language_capture = gui.capture_native_popup(w, language_popup, destination, 'spelling-languages')
    # This private fixture packages en_US only. Choose the actual last native
    # option, then prove the stored value; radio glyph OCR is not language data.
    gui.popup_keys(w, language_popup, 'End', 'Return')
    gui.framed_keys(w, 'shift+Tab')  # Language Select -> Check spelling.
    gui.SESSION.until('installed language removes visible spelling Error status',
                      lambda: unavailable_status(require_error=False))
    gui.framed_keys(w, *(['shift+Tab'] * 10))
    saved = gui.save_settings_footer(w, saved_path=preferences)
    assert saved['composer'] == {'spellcheck': True, 'spellcheck_language': 'en_US'}, saved['composer']
    gui.close_settings_navigation(w)
    upload.focus_composer(w)
    gui._composer_native(w, '', 0, 0)
    evidence.append({'unavailable_language': 'zz_ZZ', 'status_box': unavailable,
                     'saved_unavailable_composer': saved_unavailable['composer'],
                     'status_raster': status_raster, 'status_words': unavailable_words,
                     'authored_error_rgb': error_rgb,
                     'private_palette': str(palette_path), 'language_popup': str(language_capture),
                     'saved_available_composer': saved['composer'],
                     'available_status_raster': available_status_raster})

    def setting(enabled):
        saved = _policy_settings(ctx, 0, [(10, 'toggle', enabled, 'composer', 'spellcheck')])
        assert saved['composer']['spellcheck_language'] == 'en_US', saved['composer']
        upload.focus_composer(w)

    def paste(raw):
        upload.focus_composer(w)
        gui.framed_keys(w, 'ctrl+Home', 'ctrl+shift+End')
        _refresh_clipboard(ctx, raw.encode('utf-8'), 'text/plain;charset=utf-8')
        gui.framed_keys(w, 'ctrl+v')
        size = len(raw.encode('utf-8'))
        return gui._composer_native(w, raw, size, size)

    def shaped_boundaries(raw):
        gui.framed_keys(w, 'ctrl+Home')
        state = gui._composer_native(w, raw, 0, 0)
        points = {0: state['caret']}
        size = len(raw.encode('utf-8'))
        for _ in range(len(raw) + 1):
            if state['cursor'] == size:
                return points
            previous = state['cursor']
            gui.framed_keys(w, 'Right')
            state = gui.native_input_state(w)
            assert state['text'] == raw and state['cursor'] == state['anchor'] > previous, state
            points[state['cursor']] = state['caret']
        raise AssertionError(('Native caret did not traverse draft', raw, state))

    def word_box(raw, token, points, occurrence=0):
        offset = -1
        for _ in range(occurrence + 1):
            offset = raw.index(token, offset + 1)
        begin = len(raw[:offset].encode('utf-8'))
        end = begin + len(token.encode('utf-8'))
        # Native visible-boundary affinity includes adjacent zero-width IRC controls.
        while end < len(raw.encode('utf-8')) and raw.encode('utf-8')[end] in (2, 15, 17, 29, 30, 31):
            end += 1
        a, b = points[begin], points[end]
        assert a[1] == b[1] and b[0] > a[0] and a[3] == b[3], (token, a, b)
        return [a[0], a[1], b[0], a[1] + a[3]]

    def raster(raw, tokens, label):
        setting(False)
        paste(raw)
        points = shaped_boundaries(raw)
        size = len(raw.encode('utf-8'))
        before = gui._composer_native(w, raw, size, size)
        boxes = [word_box(raw, token, points, occurrence) for token, occurrence in tokens]
        disabled = gui.native_rgb_capture(w)
        save_rgb(label + '-disabled', disabled)
        setting(True)
        assert gui._composer_native(w, raw, size, size) == before
        assert shaped_boundaries(raw) == points, 'Decorations changed native shaped caret geometry'
        assert gui._composer_native(w, raw, size, size) == before
        pane = gui.read_layout(preferences)['conversation']['rect']
        pad = round(12 * ctx['args'].zoom)
        input_crop = (max(0, round(pane[0])), max(0, round(min(box[1] for box in boxes)) - pad),
                      min(disabled['width'], round(pane[0] + pane[2])),
                      min(disabled['height'], round(max(box[3] for box in boxes)) + pad))
        observed = {}

        def annotated():
            image = gui.native_rgb_capture(w)
            observed['last_image'] = image
            observed.pop('last_masks', None)
            if 'first_image' not in observed:
                observed['first_image'] = str(save_rgb(label + '-enabled-first', image))
                first_crop = destination.with_name(destination.stem + '-spelling-' + label + '-enabled-first-input.png')
                Image.frombytes('RGB', (image['width'], image['height']), image['pixels']).crop(input_crop).save(first_crop)
                captures[label + '-enabled-first-input'] = str(first_crop)
            assert (image['width'], image['height']) == (disabled['width'], disabled['height'])
            masks = []
            for box in boxes:
                left, top, right, bottom = map(round, box)
                changes = []
                for yy in range(top, bottom):
                    for xx in range(left, right):
                        index = (yy * image['width'] + xx) * 3
                        actual = tuple(image['pixels'][index:index + 3])
                        old = tuple(disabled['pixels'][index:index + 3])
                        if error_decoration(old, actual):
                            changes.append((xx, yy, actual))
                masks.append(changes)
            observed['last_masks'] = masks
            if not masks[0]:
                return False
            changes = masks[0]
            xs, ys = {item[0] for item in changes}, {item[1] for item in changes}
            left, top, right, bottom = boxes[0]
            # Error-like changes must themselves form a localized under-word
            # squiggle, not merely changed glyphs or a nonempty pixel count.
            if not (len(xs) >= (right - left) * .6 and len(ys) >= 2 and
                    min(ys) >= top + (bottom - top) / 2):
                return False
            assert all(not mask for mask in masks[1:]), ('False underline on correct/excluded text', boxes, masks)
            assert gui._composer_native(w, raw, size, size) == before
            observed.update(image=image, masks=masks)
            return True

        try:
            gui.SESSION.until('actual dictionary error squiggle and clean excluded neighbors', annotated)
        finally:
            if 'last_image' in observed:
                try:
                    last = observed['last_image']
                    save_rgb(label + '-enabled-current', last)
                    current_crop = destination.with_name(destination.stem + '-spelling-' + label + '-enabled-current-input.png')
                    Image.frombytes('RGB', (last['width'], last['height']), last['pixels']).crop(input_crop).save(current_crop)
                    captures[label + '-enabled-current-input'] = str(current_crop)
                    diagnostic = destination.with_name(destination.stem + '-spelling-' + label + '-raster-current.json')
                    masks = observed.get('last_masks', [])
                    diagnostic.write_text(json.dumps({
                        'window': w, 'pid': ctx['proc'].pid, 'expected_native': before,
                        'current_native': gui.native_input_state(w), 'input_crop': input_crop,
                        'composer_preferences': tomllib.loads(preferences.read_text())['composer'],
                        'word_boxes': boxes, 'tokens': tokens, 'authored_error_rgb': error_rgb,
                        'error_pixel_counts': [len(mask) for mask in masks],
                        'error_pixel_bounds': [
                            (min(item[0] for item in mask), min(item[1] for item in mask),
                             max(item[0] for item in mask) + 1, max(item[1] for item in mask) + 1)
                            if mask else None for mask in masks],
                    }, ensure_ascii=False, indent=2) + '\n')
                except Exception as error:
                    print('Current spelling raster evidence could not be retained:', error, file=sys.stderr)
        enabled_path = save_rgb(label + '-enabled', observed['image'])
        mask = Image.new('RGB', (disabled['width'], disabled['height']), 'black')
        for xx, yy, rgb in observed['masks'][0]:
            mask.putpixel((xx, yy), rgb)
        mask_path = destination.with_name(destination.stem + '-spelling-' + label + '-underline-mask.png')
        mask.save(mask_path)
        evidence.append({'stage': label, 'raw_utf8_hex': raw.encode('utf-8').hex(),
                         'unchanged_native_state': before, 'shaped_caret_boundaries': points,
                         'token_boxes': boxes, 'tokens': tokens, 'enabled_png': str(enabled_path),
                         'underline_mask_png': str(mask_path), 'changed_pixels': observed['masks'][0],
                         'clean_neighbor_changed_pixels': [len(mask) for mask in observed['masks'][1:]]})
        return boxes

    # wrold/world and hello are the real dictionary check fixture; no rank is assumed.
    original = 'hello\u00a0\x02wrold\x02 world'
    boxes = raster(original, [('wrold', 0), ('hello', 0), ('world', 0)], 'styled-multibyte')
    gui.framed_keys(w, 'ctrl+Home', 'ctrl+shift+End')
    selected = gui._composer_native(w, original, len(original.encode('utf-8')), 0)
    left, top, right, bottom = boxes[0]
    point = ((left + right) / 2, (top + bottom) / 2)
    def right_click():
        gui.move_pointer(w, *point)
        gui.POINTER.button(True, code=273)
        gui.POINTER.button(False, code=273)
    popup = gui.open_native_popup(w, right_click)
    placement = gui.pointer_popup_bounds(w, popup, point)
    root_capture = gui.capture_native_popup(w, popup, destination, 'spelling-word-context')
    # The actual branch is located by its rendered control label, not a menu index.
    branches, branch_mask = glyph_words(root_capture, 'context', popup['output_scale'])
    branch = [word for word in branches if word['text'] == 'Spelling']
    assert len(branch) == 1, ('Actual Spelling branch missing', branches)
    marker = gui.GUI_DEBUG[w][1].stat().st_size
    popup_click(popup, branch[0]['box'])
    submenu = gui.await_native_popup(w, marker, parent=popup)
    submenu_capture = gui.capture_native_popup(w, submenu, destination, 'spelling-suggestions')
    # The observed Home item in the pinned en_US fixture is world. Exercise the
    # native suggestion action; exact resulting raw bytes below prove its value,
    # rather than using popup glyph OCR or testing dictionary suggestion ranking.
    replacement = 'world'
    gui.popup_keys(w, submenu, 'Home', 'Return')
    corrected = original.replace('wrold', replacement, 1)
    correction_end = len(('hello\u00a0\x02' + replacement).encode('utf-8'))
    corrected_state = gui._composer_native(w, corrected, correction_end, correction_end)
    gui.command('undo', window=w)
    undone = gui._composer_native(w, original, selected['cursor'], selected['anchor'])
    assert undone == selected, ('One native Undo lost bytes/caret/nonempty selection', selected, undone)
    evidence.append({'pointer': point, 'placement': placement, 'context_png': str(root_capture),
                     'suggestions_png': str(submenu_capture),
                     'context_glyphs': branches, 'context_ocr_mask': branch_mask,
                     'suggestion_activation': 'native Home/Return in pinned en_US fixture',
                     'chosen_suggestion': replacement,
                     'original_selection': selected, 'corrected': corrected_state, 'one_undo': undone})

    nick = ctx['query_target']
    assert nick == 'guiobserver', nick  # Existing real private fixture channel member.
    exclusions = 'wrold https://wrold.invalid ' + nick + ' \x11wrold\x11'
    raster(exclusions, [('wrold', 0), ('https://wrold.invalid', 0), (nick, 0), ('wrold', 2)], 'exclusions')
    # End with an empty native draft, never submit the synthetic text.
    gui.framed_keys(w, 'ctrl+Home', 'ctrl+shift+End', 'BackSpace')
    gui._composer_native(w, '', 0, 0)
    gui.save_state(ctx['proc'], preferences, ctx['database_path'])
    with sqlite3.connect(ctx['database_path']) as database:
        sent = database.execute('SELECT text FROM messages WHERE own=1 AND (text=? OR text=? OR text=?)',
                                (original, corrected, exclusions)).fetchall()
    assert not sent, ('Checking/correction sent draft text', sent)
    token = 'spelling-private-barrier-' + str(time.monotonic_ns())
    observer.send('PING :' + token)
    observer.wait(lambda command, params, prefix: command == 'PONG' and params and params[-1] == token, timeout=15)
    frames = [frame for frame in observer.frames[wire_start:] if frame['command'] == 'PRIVMSG' and
              frame['prefix'].split('!', 1)[0] in {ctx['own_nick'], ctx['other_nick']}]
    assert not frames, ('Spelling unexpectedly sent IRC PRIVMSG', frames)
    evidence.append({'no_own_privmsg_through_server_barrier': True, 'no_draft_message_rows': True,
                     'captures': captures, 'ime_ghost': 'not exercised: fixture callback exposes no input-method handle'})
    return _refresh_return(ctx, 'spelling', evidence)


def run_unread_divider(ctx):
    """Visible, inactive native pane on the owned compositor's second output.

    Integrate as --scope unread-divider in interface_refresh_smoke.py. No extra
    client, host service, injected UI state, or synthetic read-marker mutation.
    """
    import csv
    import io
    import sqlite3

    w, proc = ctx['window'], ctx['proc']
    destination = Path(ctx['destination'])
    db, identity = ctx['database_path'], ctx['query_id']
    phrase = ['new', 'since', 'your', 'last', 'visit']
    colors = [(42, 185, 113), (179, 79, 213), (225, 127, 45)]
    bodies = ['\x042AB971,2AB971UnreadAnchorAAAAAAAA\x0f',
              '\x04B34FD5,B34FD5UnreadAnchorBBBBBBBB\x0f',
              '\x04E17F2D,E17F2DUnreadRemoteCCCCCCCC\x0f']
    draft = 'Private unread divider draft retained'
    phases = {}

    def durable():
        with sqlite3.connect(f'file:{db}?mode=ro', uri=True) as connection:
            rows = connection.execute(
                'SELECT id,nick,text,own,pending,time,presence,sort_time,seq '
                'FROM messages WHERE buffer=? ORDER BY sort_time,seq', (identity,)).fetchall()
            marker = connection.execute(
                'SELECT read_marker,unread,draft FROM buffers WHERE id=?', (identity,)).fetchone()
        return {'rows': rows, 'marker': marker[:2], 'draft': marker[2]}

    def capture(label):
        # Save the SAME frame later used for swatches, label and line assertions.
        image = gui.native_rgb_capture(w)
        native = Image.frombytes('RGB', (image['width'], image['height']), image['pixels'])
        path = destination.with_name(destination.stem + '-unread-divider-' + label + '.png')
        native.save(path)
        record = {'png': str(path), 'geometry': image['geometry'],
                  'input': gui.native_input_state(w), 'database': durable(),
                  'outputs': gui.SESSION.ipc('get_outputs'),
                  'workspaces': gui.SESSION.ipc('get_workspaces'),
                  'windows': [(node['id'], name) for node, name in
                              gui.tree_windows(gui.SESSION.ipc('get_tree'))],
                  'trace_offset': trace.stat().st_size}
        phases[label] = record
        metadata = path.with_suffix('.json')
        metadata.write_text(json.dumps(record, ensure_ascii=False, indent=2) + '\n')
        result = subprocess.run(['tesseract', str(path), 'stdout', '--psm', '11', 'tsv'],
                                check=True, capture_output=True, text=True, timeout=20)
        words = []
        px, py, pw, ph = pane
        for row in csv.DictReader(io.StringIO(result.stdout), delimiter='\t'):
            text = row.get('text', '').strip()
            if not text:
                continue
            x, y, width, height = (int(row[key]) for key in ('left', 'top', 'width', 'height'))
            if px <= x < px + pw and py <= y < py + ph:
                words.append((re.sub('[^a-z]', '', text.lower()), (x, y, x + width, y + height)))
        labels = []
        for index in range(len(words) - len(phrase) + 1):
            group = words[index:index + len(phrase)]
            if [word for word, box in group] == phrase:
                labels.append((min(box[0] for word, box in group),
                               min(box[1] for word, box in group),
                               max(box[2] for word, box in group),
                               max(box[3] for word, box in group)))
        boxes = [_policy_box(ctx, color, image=image) for color in colors]
        record.update(words=words, divider_labels=labels, swatches=boxes)
        metadata.write_text(json.dumps(record, ensure_ascii=False, indent=2) + '\n')
        return native, record

    def visible_inactive(record):
        assert record['geometry'] == geometry, ('Original window moved', record)
        original = [item for item in record['workspaces'] if item['name'] == workspace]
        assert len(original) == 1 and original[0]['visible'] and not original[0]['focused'], original
        assert original[0]['output'] == output, original
        focused = [item for item in record['workspaces'] if item['focused']]
        assert len(focused) == 1 and focused[0]['name'] == empty_workspace, focused
        assert focused[0]['output'] == created, focused
        assert (w, output) in record['windows'], record['windows']
        assert not any(name == created for node, name in record['windows']), record['windows']
        # text_input.disable clears the protocol snapshot on legitimate blur;
        # the actual draft must remain durable even when IME is not enabled.
        assert record['input'] is None or record['input']['text'] == draft, record['input']
        assert record['database']['draft'] == draft, record['database']
        assert re.search(r'wl_keyboard[@#]\d+\.leave\(\d+,\s*wl_surface[@#]\d+',
                         trace.read_bytes()[leave_offset:].decode(errors='replace'))

    _policy_effects(ctx, fx='off')
    _policy_focus(ctx, identity)
    for body in bodies[:2]:
        _policy_send(ctx, body)
    _policy_wait_box(ctx, colors[0])
    _policy_wait_box(ctx, colors[1])
    upload.focus_composer(w)
    gui.type_text(draft)
    gui._composer_native(w, draft, len(draft), len(draft))
    # Settings-driven save is allowed ONLY while the original output is focused.
    gui.save_state(proc, ctx['preferences'], db)
    upload.focus_composer(w)
    gui._composer_native(w, draft, len(draft), len(draft))
    read_before = gui.wait_read_marker(proc, db, identity)
    assert read_before[3] == bodies[1], read_before
    pane = gui.read_layout(ctx['preferences'])['conversation']['rect']
    geometry = gui.window_geometry(w)
    gui.move_pointer(w, geometry['WIDTH'] - 2, 2)
    trace = gui.GUI_DEBUG[w][1]
    original_outputs = {item['name'] for item in gui.SESSION.ipc('get_outputs') if item['active']}
    assert len(original_outputs) == 1, original_outputs
    original_workspace = next(item for item in gui.SESSION.ipc('get_workspaces') if item['focused'])
    workspace, output = original_workspace['name'], original_workspace['output']
    assert output in original_outputs
    empty_workspace = 'refresh-unread-divider-inactive'
    assert not any(item['name'] == empty_workspace for item in gui.SESSION.ipc('get_workspaces'))
    baseline, before = capture('focused-read')
    a, b, _ = before['swatches']
    assert a and b and a[3] < b[1], before
    assert not before['divider_labels'], before
    assert before['input']['text'] == draft, before['input']
    assert before['database']['draft'] == draft, before['database']
    normal_gap = b[1] - a[3]
    # Include the complete tail, not just the label: a dangling spacer must move
    # an anchor or change this stable body-column strip even when OCR sees nothing.
    strip = (max(a[0], b[0]), a[1], min(a[2], b[2]), int(pane[1] + pane[3]) - 2)
    assert strip[0] < strip[2] and strip[1] < strip[3], strip
    created = None
    leave_offset = trace.stat().st_size
    try:
        gui.SESSION.ipc('command', 'create_output')

        def discover():
            added = [item for item in gui.SESSION.ipc('get_outputs')
                     if item['active'] and item['name'] not in original_outputs]
            assert len(added) <= 1, added
            return added[0] if added else None

        new_output = gui.SESSION.until('new owned headless output', discover)
        created = new_output['name']
        right = max(item['rect']['x'] + item['rect']['width'] for item in before['outputs']
                    if item['active'])
        gui.SESSION.ipc('command', 'output ' + json.dumps(created) +
                        ' mode 640x480 position ' + str(right) + ' 0 scale 1')
        gui.SESSION.ipc('command', 'focus output ' + json.dumps(created))
        gui.SESSION.ipc('command', 'workspace ' + json.dumps(empty_workspace))

        def left_keyboard():
            assert proc.poll() is None, 'Native app exited before keyboard leave'
            leave = re.search(r'wl_keyboard[@#]\d+\.leave\(\d+,\s*wl_surface[@#]\d+',
                              trace.read_bytes()[leave_offset:].decode(errors='replace'))
            return leave and any(item['focused'] and item['name'] == empty_workspace
                                 for item in gui.SESSION.ipc('get_workspaces'))

        gui.SESSION.until('actual native keyboard leave with original workspace visible', left_keyboard)
        gui.await_native_frame(w, leave_offset,
            re.compile(r'wl_keyboard[@#]\d+\.leave\(\d+,\s*wl_surface[@#]\d+'))
        blurred, no_new = capture('inactive-no-new-data')
        visible_inactive(no_new)
        assert no_new['database'] == before['database'], (before, no_new)
        assert no_new['swatches'] == before['swatches'], (before, no_new)
        assert not no_new['divider_labels'], no_new
        assert ImageChops.difference(baseline.crop(strip), blurred.crop(strip)).getbbox() is None, (
            'Blur inserted label, spacer, or changed the read transcript', strip)

        _policy_send(ctx, bodies[2])
        _policy_wait_box(ctx, colors[2])
        # insert() commits messages before recount(); persist() does NOT write
        # buffers.unread (application.cpp:1156-1187, storage.cpp:1315-1433).
        # tick() saves the model every 30s (application.cpp:3416). Observe that
        # real save without driving Settings or changing any helper/IPC timeout.
        autosave_started = time.monotonic()
        autosave_deadline = autosave_started + 30 + gui.SESSION.timeout
        while True:
            assert proc.poll() is None, 'Native app exited before durable unread save'
            gui.SESSION.alive()
            saved_unread = durable()
            assert saved_unread['marker'][0] == read_before[0], saved_unread
            if saved_unread['marker'][1] > 0:
                break
            assert time.monotonic() < autosave_deadline, (
                'Native 30s model autosave did not persist genuine unread', saved_unread)
            time.sleep(.025)  # Poll the existing durable lifecycle, not layout timing.
        autosave_wait_seconds = time.monotonic() - autosave_started
        positive, unread = capture('inactive-real-unread')
        visible_inactive(unread)
        ua, ub, uc = unread['swatches']
        assert ua and ub and uc, unread
        assert unread['database']['marker'][0] == read_before[0], unread
        assert unread['database']['marker'][1] == 1, unread
        assert unread['database']['rows'][:-1] == before['database']['rows'], unread
        assert unread['database']['rows'][-1][2] == bodies[2], unread
        assert len(unread['divider_labels']) == 1, unread
        label = unread['divider_labels'][0]
        assert ub[3] < label[1] < label[3] < uc[1], (ub, label, uc)
        assert uc[1] - ub[3] > normal_gap, (normal_gap, ub, uc)
        # Witness the actual long horizontal native line below the label in the
        # same frame. No theme RGB or production separator-height formula copied.
        left, right = int(pane[0]) + 4, int(pane[0] + pane[2]) - 4
        pixels = positive.load()
        line_rows = []
        for y in range(label[3], uc[1]):
            counts = {}
            for x in range(left, right):
                color = pixels[x, y]
                counts[color] = counts.get(color, 0) + 1
            color, count = max(counts.items(), key=lambda item: item[1])
            if count >= (right - left) * .85 and all(
                    sum(pixels[x, other] != color for x in range(left, right)) >= (right - left) * .8
                    for other in (y - 2, y + 2)):
                line_rows.append(y)
        unread['divider_line_rows'] = line_rows
        positive_path = Path(unread['png']).with_suffix('.json')
        positive_path.write_text(json.dumps(unread, ensure_ascii=False, indent=2) + '\n')
        assert line_rows, ('No visible divider line between label and actual remote row', unread)
        # A second capture after durable persistence must still contain genuine
        # unread; focus suppression must not silently acknowledge the arrival.
        _, retained = capture('inactive-unread-retained')
        visible_inactive(retained)
        assert retained['database'] == unread['database'], retained
        assert retained['database']['marker'][1] == 1, retained
        assert retained['swatches'] == unread['swatches'], retained
        assert retained['divider_labels'] == unread['divider_labels'], retained
    finally:
        # Restore even when create_output succeeded but discovery/config failed.
        gui.SESSION.ipc('command', 'focus output ' + json.dumps(output))
        gui.SESSION.ipc('command', 'workspace ' + json.dumps(workspace))
        gui.SESSION.ipc('command', '[con_id=' + str(w) + '] focus')
        extras = [item['name'] for item in gui.SESSION.ipc('get_outputs')
                  if item['active'] and item['name'] not in original_outputs]
        assert len(extras) <= 1, extras
        for name in extras:
            assert created is None or name == created, (created, name)
            gui.SESSION.ipc('command', 'output ' + json.dumps(name) + ' disable')
        gui.SESSION.until('exact original active output set restored', lambda:
            {item['name'] for item in gui.SESSION.ipc('get_outputs') if item['active']} == original_outputs)

    read_after = gui.wait_read_marker(proc, db, identity)
    assert read_after[3] == bodies[2] and read_after[0] == read_after[4], read_after
    # Persist the real read transition only after restoring the one-output GUI.
    gui.save_state(proc, ctx['preferences'], db)
    upload.focus_composer(w)
    gui._composer_native(w, draft, len(draft), len(draft))
    final, returned = capture('returned-read')
    ra, rb, rc = returned['swatches']
    assert ra and rb and rc, returned
    assert not returned['divider_labels'], returned
    # Independently rounded native row edges may differ by one physical pixel.
    assert abs((rc[1] - rb[3]) - normal_gap) <= 1, (normal_gap, rb, rc)
    assert abs((rb[1] - ra[3]) - normal_gap) <= 1, (normal_gap, ra, rb)
    assert returned['database']['rows'] == unread['database']['rows'], returned
    assert returned['database']['marker'] == (read_after[4], 0), returned
    assert returned['input']['text'] == draft, returned['input']
    assert returned['database']['draft'] == draft, returned['database']
    return _refresh_return(ctx, 'unread-divider', {
        'cli_scope': '--scope unread-divider', 'phases': phases,
        'original_outputs': sorted(original_outputs), 'created_output': created,
        'original_workspace': workspace, 'inactive_workspace': empty_workspace,
        'read_before': read_before, 'read_after': read_after,
        'durable_unread_wait_seconds': autosave_wait_seconds,
        'durable_unread_wait_bound_seconds': 30 + gui.SESSION.timeout,
        'normal_interbody_gap': normal_gap, 'unchanged_read_tail_strip': strip,
        'draft': draft, 'raw_history_preserved': True,
        'native_keyboard_leave_observed': True, 'original_active_outputs_restored': True,
        'exclusion_coverage': 'tests/transcript_check.cpp visitSeparator',
    })


def run_settings_tree(ctx):
    """Run only the native Settings tree workflow, with its normal restoration."""
    return run_settings_refinements(ctx, tree_only=True)


def run_settings_refinements(ctx, *, tree_only=False):
    """Real private Settings controls, native byte selections and actual editor commands."""
    w, p = ctx['window'], ctx['proc']
    prefs, db = Path(ctx['preferences']), ctx['database_path']
    destination = Path(ctx['destination'])
    scale = ctx['args'].zoom
    evidence, captures = [], []
    initial_size = gui.window_geometry(w)
    search_query = ''

    def capture(label):
        path = gui.capture_window(w, destination, 'settings-refinements-' + label)
        captures.append(str(path))
        return str(path)

    def region():
        return gui.read_layout(prefs)['settings']['rect']

    def caption_box(label):
        bounds = region()
        if label.casefold() not in ('search settings', 'matching settings'):
            # A query's text is not a setting caption, even when it says the
            # same words. Restrict form captions below the visible search chrome.
            matching = upload.label_rect(w, destination, 'Matching settings',
                                         region=bounds, exact_caption=True)
            boundary = matching or upload.label_rect(w, destination, 'Search settings',
                                                     region=bounds, exact_caption=True)
            if boundary is None:
                return None
            rx, ry, rw, rh = bounds
            top = boundary[1] + boundary[3]
            if matching is None:
                # Empty queries hide the whole results group; the body begins
                # below the Search input's bounded native caption band instead.
                top += max(56 * scale, 56)
            # Exclude the new tree from detail-caption OCR; its adjacent rows
            # are unrelated text columns, not part of the form's captions.
            compact = rw < 760 * scale or rh < 440 * scale
            inset = 0 if compact else 260 * scale  # panel padding + sidebar + body gap
            bounds = (rx + inset, top, rw - inset, ry + rh - top)
        box = upload.label_rect(w, destination, label, region=bounds, exact_caption=True)
        if box is None and '–' in label:
            # Tesseract renders the visible range dash as ASCII "-", as in
            # "(1-3650)"; retain the entire exact caption rather than fuzzy OCR.
            box = upload.label_rect(w, destination, label.replace('–', '-'),
                                    region=bounds, exact_caption=True)
        return box

    def field(label, expected=None):
        # Locate ordinary user-facing labels, never font names or technical IDs.
        box = gui.SESSION.until('visible Settings field ' + label,
            lambda: caption_box(label))
        x, y, width, height = box
        following = {'search settings': 'Matching settings',
                     'ui font family': 'Installed UI fonts',
                     'message font family': 'Installed message fonts'}.get(label.casefold())
        rx, ry, rw, rh = region()
        stop = y + height + max(56 * scale, 56)
        if following:
            next_box = caption_box(following)
            if next_box is not None:
                stop = next_box[1]
        def focused():
            state = gui.native_input_state(w)
            if state is None or state['caret'] is None:
                return False
            # A focus round trip can scroll the real Input fully into view.
            # Validate against its freshly observed caption, not the old pixels.
            fresh_box = caption_box(label)
            if fresh_box is None:
                return False
            fresh_x, fresh_y, _, fresh_height = fresh_box
            next_box = caption_box(following) if following else None
            fresh_stop = (next_box[1] if next_box is not None else
                          fresh_y + fresh_height + max(56 * scale, 56))
            cx, cy, cw, ch = state['caret']
            if not (fresh_y + fresh_height <= cy < fresh_stop):
                return False
            rx, ry, rw, rh = region()
            if not (fresh_x - 4 * scale <= cx and cx + cw <= rx + rw):
                return False
            return state if expected is None or state['text'] == expected else False
        # Font metrics can enlarge the gap between caption and Input. Stay in
        # the observed caption band and require actual native text/caret proof.
        probes = [round((y + height + stop) / 2),
                  *range(round(y + height + 1), round(stop), max(1, round(height / 2)))]
        for probe_y in probes:
            gui.click(w, x + min(width / 2, 24 * scale), probe_y)
            gui.framed_keys(w, 'Tab', 'shift+Tab')
            state = focused()
            if state:
                return state
        raise AssertionError(('No matching native input in visible caption band', label, expected, box, stop))

    def replace(value, label):
        return gui.settings_field(w, value, destination, 'settings-refinements-' + label)

    def query(value, label):
        nonlocal search_query
        field('Search settings', search_query)
        gui.framed_keys(w, 'ctrl+a', 'BackSpace')
        # Separate real edits catch a query field destroyed/recreated by filtering.
        for index, character in enumerate(value):
            gui.type_text(character)
            prefix = value[:index + 1]
            end = len(prefix.encode('utf-8'))
            gui._composer_native(w, prefix, end, end)
        end = len(value.encode('utf-8'))
        search_query = value
        gui.framed_keys(w, 'Return')
        state = gui.observe_native_input(w, destination,
            'settings-refinements-search-' + label, value, end, end)
        evidence.append({'search_query': value, 'after_jump': state})
        return state

    def jump(value, label, caption, expected=None):
        query(value, label)
        # Seeing the target in the native detail viewport proves jump/scroll;
        # filtering alone cannot satisfy this observation.
        return field(caption, expected)

    def save(label):
        # Search has a stable native route to the sticky footer; locating its
        # plain caption avoids OCR of dark Save text on a pastel button.
        field('Search settings', search_query)
        result = gui.save_settings_footer(w, saved_path=prefs)
        # Save can rebuild the detail viewport at its top. Search retains its
        # exact query/focus: activate the real jump again before reading a field.
        gui.framed_keys(w, 'Return')
        capture(label)
        return result

    def focus_composer():
        layout = gui.read_layout(prefs)
        metrics = gui.pane_metrics(layout, scale)
        x, y, width, height = layout['conversation']['rect']
        gui.click(w, x + metrics['left'] + 16 * scale,
                  y + height - metrics['padding_y'] - 16 * scale)
        gui.framed_keys(w, 'ctrl+End')

    def close():
        upload.click_label(w, destination, 'Close', region=region())
        # Prove a fresh real composer transaction before updating bookkeeping.
        focus_composer()
        gui.observe_native_input(w, destination, 'settings-refinements-closed-composer', '', 0, 0)
        gui.SETTINGS_OPEN[w] = False

    baseline = gui.save_preferences(p, prefs, w)
    focus_composer()
    gui.framed_keys(w, 'ctrl+a', 'BackSpace')
    gui._composer_native(w, '', 0, 0)
    ui_original, message_original = baseline['appearance']['ui_font'], baseline['appearance']['message_font']
    gui.open_settings(w, prefs)
    gui.wait_layout(p, prefs, ('settings',))

    # Search must reach a leaf on a category that has not been visited. Keep an
    # invalid native edit alive while the tree and compact selector hide it.
    retention = baseline['history']['retention_days']
    jump('retention days', 'unvisited-history', 'Retention days (1–3650)', str(retention))
    replace('not-a-number', 'invalid-retention')
    query('', 'clear-before-tree')
    gui.settings_category(w, scale, 0)
    gui.framed_keys(w, 'Right')
    gui.move_pointer(w, 8, initial_size['HEIGHT'] - 8)
    tree_root = capture('keyboard-root')
    gui.framed_keys(w, 'Right', 'Down')  # Expanded first root -> second leaf (fonts).
    gui.move_pointer(w, 8, initial_size['HEIGHT'] - 8)
    gui.framed_keys(w, 'Tab', 'End')
    gui._composer_native(w, ui_original, len(ui_original.encode('utf-8')),
                         len(ui_original.encode('utf-8')))
    gui.framed_keys(w, 'shift+Tab')
    tree_leaf = capture('keyboard-font-leaf')
    # Target the actual changed native row, without OCR of tree captions.
    # Native detail-input proof above also lets the selection paint settle.
    rx, ry, rw, rh = region()
    with Image.open(tree_root) as root_image, Image.open(tree_leaf) as leaf_image:
        crop = (round(rx + 40 * scale), round(ry + 60 * scale),
                round(rx + 220 * scale), round(ry + rh - 72 * scale))
        difference = ImageChops.difference(root_image.convert('RGB').crop(crop),
                                          leaf_image.convert('RGB').crop(crop))
        rows = [y for y in range(difference.height)
                if difference.crop((0, y, difference.width, y + 1)).getbbox()]
    assert rows, 'Native tree selection did not paint a leaf'
    first = rows[-1]
    while first - 1 in rows:
        first -= 1
    point = (crop[0] + (crop[2] - crop[0]) / 2,
             crop[1] + (first + rows[-1]) / 2)
    gui.framed_keys(w, 'Left', 'Left', 'Right')
    gui.click(w, *point)
    gui.framed_keys(w, 'Tab', 'End')
    gui._composer_native(w, ui_original, len(ui_original.encode('utf-8')),
                         len(ui_original.encode('utf-8')))
    gui.framed_keys(w, 'shift+Tab', 'Left', 'Left', 'End', 'Home')
    try:
        gui.resize_panel_viewport(w, (round(680 * scale), round(620 * scale)))
        gui.framed_keys(w, 'Tab')  # Compact selector -> Search, one nav tab stop.
        gui._composer_native(w, '', 0, 0)
        gui.settings_category(w, scale, 2, destination)
        capture('compact-root-selected')
        gui.framed_keys(w, 'shift+Tab')  # Search -> native location selector.
        popup = gui.open_native_popup(w, lambda: gui.key('Return'))
        gui.capture_native_popup(w, popup, destination, 'settings-refinements-compact-leaf-paths')
        # The first seven options retain public root indices, followed by
        # real leaves. Select the second leaf (fonts) through the native popup.
        gui.popup_keys(w, popup, 'Home', *(['Down'] * 8), 'Return', root_changed=False)
        gui.framed_keys(w, 'Tab')
        gui._composer_native(w, '', 0, 0)
        gui.framed_keys(w, 'Tab', 'End')
        gui._composer_native(w, ui_original, len(ui_original.encode('utf-8')),
                             len(ui_original.encode('utf-8')))
        capture('compact-leaf-selected')
        gui.framed_keys(w, 'shift+Tab')  # Return to Search before resizing.
    finally:
        gui.resize_panel_viewport(w, (initial_size['WIDTH'], initial_size['HEIGHT']))
    jump('retention days', 'invalid-retained-after-navigation',
         'Retention days (1–3650)', 'not-a-number')
    # Correct only the real integer control, then require a fresh explicit Save
    # write with a changed value; old preferences cannot satisfy this receipt.
    corrected = retention + 1 if retention < 3650 else retention - 1
    replace(str(corrected), 'retention-corrected')
    assert save('tree-edit-saved')['history']['retention_days'] == corrected
    field('Retention days (1–3650)', str(corrected))
    replace(str(retention), 'retention-restored')
    restored_tree = save('tree-edit-restored')
    assert restored_tree['history'] == baseline['history']
    evidence.append({'tree_pointer': point, 'tree_keyboard': ['Right', 'Down', 'Left', 'Home', 'End'],
                     'unvisited_leaf_search': True, 'invalid_edit_survived_navigation': True,
                     'compact_root_and_leaf': True, 'explicit_save_changed_and_restored': True,
                     'retention_before': retention, 'retention_saved': corrected,
                     'retention_restored': restored_tree['history']['retention_days']})
    # Exercise the real weight Selects, including their separate native popup
    # surfaces. Fresh saves prove each setting changes without changing its peer.
    original_weights = tuple(baseline['appearance'][key]
                             for key in ('ui_font_weight', 'message_font_weight'))

    def choose_weight(caption, weight, label):
        query(caption, label)
        x, y, width, height = gui.SESSION.until('visible Settings selector ' + caption,
                                              lambda: caption_box(caption))
        popup = gui.open_native_popup(
            w, lambda: gui.click(w, x + min(width / 2, 24 * scale),
                                 y + height + 20 * scale))
        gui.capture_native_popup(w, popup, destination, 'settings-weight-' + label)
        gui.popup_keys(w, popup, 'Home', *(['Down'] * ((weight - 300) // 100)),
                       'Return', root_changed=False)
        capture('weight-' + label + '-live')

    def weights_saved(label, expected):
        saved = save(label)
        appearance = saved['appearance']
        actual = tuple(appearance[key] for key in ('ui_font_weight', 'message_font_weight'))
        assert actual == expected, (label, actual, expected)
        for key in ('ui_font', 'message_font', 'scale', 'message_scale'):
            assert appearance[key] == baseline['appearance'][key], (label, key)
        evidence.append({'native_weight_select': label, 'ui_weight': actual[0],
                         'message_weight': actual[1], 'fresh_save': True})
        return saved

    choose_weight('UI font weight', 700, 'ui-bold')
    weights_saved('ui-weight-saved', (700, original_weights[1]))
    choose_weight('Message font weight', 300, 'message-light')
    weights_saved('message-weight-saved', (700, 300))
    choose_weight('UI font weight', original_weights[0], 'ui-restored')
    weights_saved('ui-weight-restored', (original_weights[0], 300))
    choose_weight('Message font weight', original_weights[1], 'message-restored')
    restored_tree = weights_saved('weights-restored', original_weights)
    if tree_only:
        assert restored_tree['appearance'] == baseline['appearance']
        assert restored_tree['bindings'] == baseline['bindings']
        close()
        upload.focus_composer(w)
        gui._composer_native(w, '', 0, 0)
        return {'scope': 'settings-tree', 'native_consumer': True,
                'private_fixture_only': True, 'evidence': evidence, 'captures': captures,
                'limitations': ['Geometry still requires focused visual inspection of the captured native rasters.',
                                'The legacy font and command matrix remains in settings-refinements.']}
    # These are fuzzy subsequences of actual setting labels, not current values.
    jump('UI fnt fmly', 'ui-family', 'UI font family', ui_original)
    custom_ui = 'Private UI family Ω'
    replace(custom_ui, 'arbitrary-ui-family')
    gui.framed_keys(w, 'Return')
    jump('Msg fnt fmly', 'message-family', 'Message font family', message_original)
    custom_message = 'Private message family λ'
    replace(custom_message, 'arbitrary-message-family')
    gui.framed_keys(w, 'Return')
    jump('UI fnt fmly', 'live-value-retained', 'UI font family', custom_ui)

    families = subprocess.run(['fc-list', '--format', '%{family}\n'], check=True,
        capture_output=True, text=True, timeout=15).stdout
    installed = {part.strip() for line in families.splitlines() for part in line.split(',') if part.strip()}
    candidates = sorted(name for name in installed if len(name) >= 6 and name.isascii())
    assert candidates, 'Private native fixture needs an installed Fontconfig family'
    font_query = candidates[0][::2]
    assert font_query, candidates[0]
    for kind, original in (('UI', custom_ui), ('message', custom_message)):
        caption = kind + ' font family'
        jump(caption, 'filter-' + kind, caption, original)
        replace(font_query, 'font-filter-' + kind)
        # Typing filters only; do not press Enter before this real save.
        saved = save('filter-only-' + kind)
        assert (saved['appearance']['ui_font'], saved['appearance']['message_font']) == (custom_ui, custom_message), saved
        staged = field(caption)
        assert staged['text'] in (font_query, original), (kind, staged)
        if staged['text'] != font_query:
            replace(font_query, 'font-filter-restaged-' + kind)
        gui.framed_keys(w, 'Tab')
        popup = gui.open_native_popup(w, lambda: gui.key('Return'))
        gui.capture_native_popup(w, popup, destination, 'settings-refinements-font-options-' + kind)
        gui.popup_keys(w, popup, 'Home', 'Return', root_changed=False)
        # A real selection is allowed to apply; we do not OCR the chosen family.
        gui.framed_keys(w, 'Tab', 'shift+Tab')
        selected = save('font-selected-' + kind)
        name = selected['appearance']['ui_font' if kind == 'UI' else 'message_font']
        assert name in installed, (kind, name)
        iterator = iter(name.casefold())
        assert all(any(actual == wanted for actual in iterator) for wanted in font_query.casefold()), (font_query, name)
        other_key, other_value = ('message_font', custom_message) if kind == 'UI' else ('ui_font', custom_ui)
        assert selected['appearance'][other_key] == other_value, selected
        evidence.append({'font_filter': kind, 'query': font_query, 'selected': name,
                         'filter_did_not_apply': True, 'other_family_unchanged': True})
        # Restore only this actual family control before the independent picker.
        jump(kind + ' font family', 'restore-custom-' + kind, kind + ' font family', name)
        replace(original, 'restore-custom-' + kind)
        gui.framed_keys(w, 'Return')

    action = 'Transpose characters'
    original_binding = baseline['bindings']['transpose-chars']
    jump(action, 'transpose-binding', action, original_binding)
    # The current field, recorder and reset are adjacent native tab stops.
    gui.framed_keys(w, 'Tab', 'Return', 'F12', 'Tab', 'Escape', 'ctrl+g')
    # gui.key treats Ctrl+G as modal dismissal, but recorder cancellation is not.
    gui.SETTINGS_OPEN[w] = True
    gui.framed_keys(w, 'shift+Tab', 'End')
    end = len(original_binding.encode('utf-8'))
    gui._composer_native(w, original_binding, end, end)
    assert save('record-cancelled')['bindings']['transpose-chars'] == original_binding
    field(action, original_binding)
    gui.framed_keys(w, 'Tab', 'Return', 'F12', 'ctrl+Return', 'shift+Tab', 'End')
    gui._composer_native(w, 'f12', 3, 3)
    assert save('record-applied')['bindings']['transpose-chars'] == 'f12'
    close()
    upload.focus_composer(w)
    gui.framed_keys(w, 'ctrl+a', 'BackSpace')
    source, transposed = 'aé猫', 'a猫é'
    gui.type_text(source)
    source_end = len(source.encode('utf-8'))
    gui._composer_native(w, source, source_end, source_end)
    gui.framed_keys(w, 'F12')
    end = len(transposed.encode('utf-8'))
    state = gui.observe_native_input(w, destination, 'settings-refinements-transpose-consumer', transposed, end, end)
    evidence.append({'command': 'transpose-chars', 'source': source, 'result': state})
    gui.framed_keys(w, 'ctrl+slash')
    gui._composer_native(w, source, source_end, source_end)
    # Native selection is measured in UTF-8 bytes, not codepoints or OCR.
    gui.framed_keys(w, 'shift+Left')
    selected = gui._composer_native(w, source, len('aé'.encode('utf-8')), source_end)
    gui.framed_keys(w, 'F12')
    assert gui._composer_native(w, source, selected['cursor'], selected['anchor']) == selected
    evidence.append({'transpose_selection_preserved': selected})
    gui.framed_keys(w, 'ctrl+g')
    gui.framed_keys(w, 'ctrl+End')
    gui._composer_native(w, source, source_end, source_end)
    gui.command('backward-kill-line', window=w)
    killed = gui._composer_native(w, '', 0, 0)
    gui.framed_keys(w, 'ctrl+slash')
    gui._composer_native(w, source, source_end, source_end)
    gui.command('redo', window=w)
    redone = gui._composer_native(w, '', 0, 0)
    evidence.append({'command': 'backward-kill-line', 'source': source, 'result': killed,
                     'undo_restored_raw_bytes': True, 'redo': redone})

    gui.open_settings(w, prefs)
    jump(action, 'reset-binding', action, 'f12')
    gui.framed_keys(w, 'Tab', 'Tab', 'Return', 'shift+Tab', 'shift+Tab', 'End')
    gui._composer_native(w, original_binding, len(original_binding.encode('utf-8')), len(original_binding.encode('utf-8')))
    assert save('binding-reset')['bindings']['transpose-chars'] == original_binding
    # Inspect two actual row control columns in wide and compact native rasters.
    # Their geometry is checked by the reviewer from these screenshots, not
    # technical-name OCR or source/default/scene-hierarchy assertions.
    capture('keyboard-wide-aligned-rows')
    try:
        gui.resize_panel_viewport(w, (round(680 * scale), round(620 * scale)))
        query(action, 'compact-jump')
        field(action, original_binding)
        capture('keyboard-compact-wrapped-row')
    finally:
        gui.resize_panel_viewport(w, (initial_size['WIDTH'], initial_size['HEIGHT']))
    for caption, value, current in (('UI font family', ui_original, custom_ui),
                                    ('Message font family', message_original, custom_message)):
        # Cleanup is not a search-jump assertion. Font reflow may move captions,
        # so restore through native focus and the distinct committed family value.
        query(caption, 'restore-' + caption.replace(' ', '-'))
        for _ in range(64):
            gui.framed_keys(w, 'Tab')
            state = gui.native_input_state(w)
            if state and state['text'] == current and state['caret']:
                cx, cy, cw, ch = state['caret']
                rx, ry, rw, rh = region()
                if rx <= cx < cx + cw <= rx + rw and ry <= cy < cy + ch <= ry + rh:
                    break
        else:
            raise AssertionError(('Native family restoration Input not found', caption, current))
        replace(value, 'restore-family')
        gui.framed_keys(w, 'Return')
    restored = save('restored')
    assert (restored['appearance']['ui_font'], restored['appearance']['message_font']) == (ui_original, message_original)
    assert restored['bindings'] == baseline['bindings'], restored['bindings']
    close()
    upload.focus_composer(w)
    gui._composer_native(w, '', 0, 0)
    return {'scope': 'settings-refinements', 'native_consumer': True,
            'private_fixture_only': True, 'evidence': evidence, 'captures': captures,
            'limitations': ['Row alignment/wrapping requires focused visual inspection of captured rasters.',
                'No technical command/font OCR benchmark, result-order pin, source/hierarchy pin or model injection.',
                'Invalid profile fields and IME composition retain their existing native probes; this scope checks invalid history text across tree navigation.',
                'Transpose uses a recorded binding; backward-kill-line/redo use the real command palette. Other configurable commands remain focused editor checks.']}


# Integration slice for tests/interface_refresh_smoke.py; imports already exist there.
# Add "transcript-refinements" to the scope choices and dispatch this function.
# Native raster evidence only: no OCR, scene inspection, injected rows, or font pins.
# Run in its own private fixture, like existing scopes: appearance/style settings
# intentionally remain fixture-local. Output mode is restored on body failure.
def run_transcript_refinements(ctx):
    import contextlib
    import os
    from PIL import ImageDraw

    p, w, prefs, db = ctx['proc'], ctx['window'], ctx['preferences'], ctx['database_path']
    identity, channel = ctx['channel_id'], ctx['channel']
    original_size = gui.window_geometry(w)
    original = tomllib.loads(Path(prefs).read_text())
    _policy_focus(ctx, identity)
    _policy_effects(ctx, fx='off')
    _policy_chat(ctx, fools=[])
    _policy_appearance(ctx, wrap=80)
    gui.set_navigation(p, w, prefs, db, 'hidden', 'refinement-transcript')
    gui.set_people(p, w, prefs, db, 'hidden')
    nonce = os.urandom(3).hex()
    colors = [(42, 185, 113), (179, 79, 213), (45, 155, 217),
              (217, 69, 119), (193, 151, 39), (69, 199, 181), (211, 113, 45)]
    end_color = (113, 91, 219)
    raw_rows, decoded_rows = [], []
    evidence = {'frames': [], 'limitations': []}

    def native(image):
        return Image.frombytes('RGB', (image['width'], image['height']), image['pixels'])

    def observe(label, indices):
        upload.focus_composer(w)
        gui.move_pointer(w, 0, 0)
        def settled():
            image = gui.native_rgb_capture(w)
            rect = gui.read_layout(prefs)['conversation']['rect']
            groups = [gui.native_marker_components(image, rect, colors[i]) for i in indices]
            if any(len(items) != 1 or not items[0]['full'] for items in groups):
                return None
            return image, [items[0]['bbox'] for items in groups]
        image, boxes = gui.SESSION.until(label + ' complete actual body swatches', settled)
        assert all(a[3] < b[1] for a, b in zip(boxes, boxes[1:])), boxes
        rect = tuple(map(round, gui.read_layout(prefs)['conversation']['rect']))
        x, y, width, height = rect
        raster = native(image)
        pane = raster.crop((x, y, x + width, y + height))
        # Observe the actual predominant canvas, never a theme RGB constant.
        canvas = max(pane.getcolors(pane.width * pane.height), key=lambda item: item[0])[1]
        difference = ImageChops.difference(pane, Image.new('RGB', pane.size, canvas))
        red, green, blue = difference.split()
        mask = ImageChops.lighter(ImageChops.lighter(red, green), blue).point(lambda v: 255 if v else 0)
        path = _policy_capture(ctx, 'refinement-' + label)
        result = {'label': label, 'client': gui.window_geometry(w), 'pane_xywh': rect,
                  'body_marker_ltrb': boxes, 'canvas_rgb': canvas, 'capture': str(path)}
        evidence['frames'].append(result)
        return raster, mask, boxes, rect, result

    def plate(mask, box, rect):
        # The swatch is known to be inside the body. Its connected non-canvas
        # component must include the complete plate, not just marker ink.
        x, y, width, height = rect
        seed = ((box[0] + box[2]) // 2 - x, (box[1] + box[3]) // 2 - y)
        connected = mask.copy()
        ImageDraw.floodfill(connected, seed, 128)
        component = connected.point(lambda value: 255 if value == 128 else 0)
        found = component.getbbox()
        if found is None:
            return None
        left, top, right, bottom = found
        actual = (x + left, y + top, x + right, y + bottom)
        # A component reaching pane edges is backdrop/connected scene paint;
        # an isolated swatch is not evidence of a painted bubble surface.
        if not (0 < left < right < width and 0 < top < bottom < height and
                actual[0] < box[0] and actual[1] < box[1] and
                actual[2] > box[2] and actual[3] > box[3]):
            return None
        return actual

    with contextlib.ExitStack() as stack:
        stack.callback(gui.resize_panel_viewport, w,
                       (original_size['WIDTH'], original_size['HEIGHT']))
        gui.resize_panel_viewport(w, (1280, 960))
        clients = []
        for prefix in ('qa', 'qb', 'qf'):
            nick = prefix + nonce
            client = stack.enter_context(gui.fixtures.BootstrapClient(
                dict(ctx['lab'].endpoints['direct'], nick=nick, username=nick),
                ctx['lab'].ca_file, authenticate=False))
            client.send('JOIN ' + channel)
            gui.receive_until(ctx['observer'], 'JOIN', [channel], nick, redact=ctx['lab']._redact)
            clients.append((nick, client))
        gui.wait_read_marker(p, db, identity)

        def send(index, sender, middle):
            nick, client = clients[sender]
            color = ''.join(f'{v:02X}' for v in colors[index])
            end = ''.join(f'{v:02X}' for v in end_color)
            raw = '\x04' + color + ',' + color + 'LEFTLEFT\x0f' + middle
            raw += '\x04' + end + ',' + end + 'RGHTRGHT\x0f'
            decoded = 'LEFTLEFT' + middle + 'RGHTRGHT'
            assert len(('PRIVMSG ' + channel + ' :' + raw).encode()) <= 510
            client.send(f'PRIVMSG {channel} :{raw}')
            gui.receive_until(ctx['observer'], 'PRIVMSG', [channel, raw], nick,
                              redact=ctx['lab']._redact)
            ctx['expected_messages'].extend(ctx['channel_messages'](raw, nick))
            gui.wait_messages(p, db, ctx['expected_messages'])
            raw_rows.append(raw)
            decoded_rows.append(decoded)
            return raw

        # A/A/B/A/A: returning A must not continue its interrupted group.
        for i, (sender, middle) in enumerate(((0, ' café e\u0301 '), (0, ' x '),
                (1, ' café e\u0301 '), (0, ' café e\u0301 '), (0, ' longer body ' * 3))):
            send(i, sender, middle)
            gui.wait_read_marker(p, db, identity)
        gui.set_message_style(p, w, prefs, db, ctx['args'], ctx['destination'], identity,
                              'bubbles', label='refinement-bubbles')
        _, mask, boxes, rect, frame = observe('bubbles-wide', range(5))
        surfaces = [plate(mask, box, rect) for box in boxes]
        frame['candidate_surface_ltrb'] = surfaces
        if all(surfaces):
            frame['candidate_surface_gaps'] = [
                b[1] - a[3] for a, b in zip(surfaces, surfaces[1:])]
            frame['candidate_surface_widths'] = [box[2] - box[0] for box in surfaces]
        evidence['limitations'].append('Bubble connected-component bounds are candidates, not '
            'independently identified material boundaries: antialiasing/avatar/background paint can '
            'connect components. Inspect saved native plate edges for fit and relative '
            'message/group gaps in A/A/B/A/A order; no fixed pixel sizes are asserted. '
            'If material edges cannot be identified, bubble geometry acceptance remains unproven.')

        # Real native row-menu Copy checks the whole decoded source, including
        # combining marks, independently of wrapping/selection already covered.
        for i, box in enumerate(boxes):
            gui.seed_clipboard('private-row-copy-sentinel-' + str(i), ctx['cleanup'])
            popup = gui.open_native_popup(w, lambda box=box: (
                gui.move_pointer(w, (box[0] + box[2]) / 2, (box[1] + box[3]) / 2),
                gui.POINTER.button(True, code=273), gui.POINTER.button(False, code=273)))
            gui.popup_keys(w, popup, 'Home', 'Return', root_changed=False)
            gui.SESSION.until('exact native decoded row copy',
                              lambda i=i: gui.clipboard_text() == decoded_rows[i])
        frame['copied_rows'] = decoded_rows[:]

        gui.set_message_style(p, w, prefs, db, ctx['args'], ctx['destination'], identity,
                              'irc', label='refinement-irc')
        _, _, wide, wide_rect, _ = observe('irc-wide', range(5))
        gui.set_message_style(p, w, prefs, db, ctx['args'], ctx['destination'], identity,
                              'stacked', label='refinement-stacked')
        _, _, stacked, stacked_rect, stacked_frame = observe('stacked-wide', range(5))
        assert stacked_rect == wide_rect, (stacked_rect, wide_rect)
        assert max(b[0] for b in stacked) - min(b[0] for b in stacked) <= 2, stacked
        # If adaptive IRC is inline at this width, forced stacked moves bodies
        # left and gives every row a metadata line, including repeated senders.
        if wide[0][0] > stacked[0][0] + 2:
            assert all(s[0] < i[0] for s, i in zip(stacked, wide)), (stacked, wide)
            assert all(stacked[n + 1][1] - stacked[n][1] > wide[n + 1][1] - wide[n][1]
                       for n in range(3)), (stacked, wide)
        stacked_frame['body_x_from_pane'] = [b[0] - stacked_rect[0] for b in stacked]
        stacked_frame['metadata_visual_review'] = (
            'Inspect metadata above every body at wide width, including repeated senders; '
            'compare with adaptive IRC at the same width.')
        gui.set_message_style(p, w, prefs, db, ctx['args'], ctx['destination'], identity,
                              'irc', label='refinement-irc-restored')
        gui.resize_panel_viewport(w, (720, 960))
        _, _, compact, compact_rect, _ = observe('irc-compact-hidden-panes', range(5))
        same_client = gui.window_geometry(w)
        gui.set_navigation(p, w, prefs, db, 'expanded', 'refinement-narrow-pane')
        gui.set_people(p, w, prefs, db, 'expanded')
        _, _, narrow, narrow_rect, _ = observe('irc-same-client-expanded-panes', range(5))
        assert gui.window_geometry(w) == same_client, 'Pane test changed outer client geometry'
        assert narrow_rect[2] < compact_rect[2] < wide_rect[2], (narrow_rect, compact_rect, wide_rect)
        evidence['metadata_visual_review'] = {
            'required': 'Inspect all five native IRC rows: narrow metadata above EVERY body, '
                        'including second/fifth same-sender rows; wide metadata inline. '
                        'Compare compact hidden/expanded panes at identical outer dimensions.',
            'body_x_from_pane': {'wide': [b[0] - wide_rect[0] for b in wide],
                'compact': [b[0] - compact_rect[0] for b in compact],
                'narrow': [b[0] - narrow_rect[0] for b in narrow]}}
        evidence['limitations'].append('Color swatches locate original body ink, not metadata identities. '
            'Timestamp/avatar/nick placement is native raster review, not an OCR or hierarchy assertion. '
            'No fixed pane-width transition is assumed; if captures do not straddle it, acceptance remains unproven.')

        # Keep IRC wide so metadata cannot be mistaken for summary ink between flanks.
        gui.resize_panel_viewport(w, (1280, 960))
        gui.set_navigation(p, w, prefs, db, 'hidden', 'refinement-fool-flanks')
        gui.set_people(p, w, prefs, db, 'hidden')
        send(5, 2, ' concealed private body ')
        send(6, 1, ' after summary ')
        _policy_chat(ctx, fools=[clients[2][0]])
        _policy_appearance(ctx, fool='minimized')
        raster, mask, flanks, rect, frame = observe('minimized-flanked', (4, 6))
        before, after = flanks
        x, y, width, height = rect
        band = mask.crop((0, before[3] - y, width, after[1] - y))
        # Record raster separation without pretending all non-canvas pixels are
        # summary glyphs: actual row surfaces can also enter this band.
        occupied = [row for row in range(band.height)
                    if band.crop((0, row, width, row + 1)).getbbox()]
        assert occupied, 'No visible paint between real minimized-summary flanks'
        ink = band.getbbox()
        summary = (x + ink[0], before[3] + ink[1], x + ink[2], before[3] + ink[3])
        frame['intervening_candidate_ink_ltrb'] = summary
        frame['blank_rows_before_after'] = [occupied[0], band.height - 1 - occupied[-1]]
        gui.click(w, (summary[0] + summary[2]) / 2, (summary[1] + summary[3]) / 2)
        revealed = _policy_wait_box(ctx, colors[5])
        frame['revealed_original_marker'] = revealed
        evidence['limitations'].append('Reveal proves a genuine minimized summary hit. '
            'The intervening ink box can include other row paint; blank-row counts alone do not '
            'prove complete summary/nonneighbor bounds. Inspect minimized/revealed native captures '
            'for nonoverlap; if edges cannot be separated, nonoverlap acceptance remains unproven.')
        _policy_capture(ctx, 'refinement-minimized-revealed')
        evidence['irc_raw_rows'] = raw_rows
        gui.wait_messages(p, db, ctx['expected_messages'])
        # The chosen quote style and selection outline are native raster review;
        # exact document/selection and actual reply routing are executable checks.
        _policy_chat(ctx, fools=[])
        observer = ctx['observer']
        if 'message-tags' not in observer.capabilities:
            observer.send('CAP REQ :message-tags')
            _, acknowledged, _ = observer.wait(lambda command, params, prefix:
                command == 'CAP' and ('ACK' in params or 'NAK' in params))
            assert 'message-tags' in observer.capabilities, acknowledged
        # Reply is a client tag, not an ISUPPORT capability. The actual sent
        # reply below must carry its source ID on the real observer wire.
        reply_evidence = []
        for style in ('bubbles', 'irc', 'stacked'):
            gui.set_message_style(p, w, prefs, db, ctx['args'], ctx['destination'], identity,
                                  style, label='refinement-reply-' + style)
            upload.focus_composer(w)
            captures = []
            own_short = 'Own short café ' + nonce + ' ' + style
            own_wrapped = ('Own selection boundary café ' +
                           'A self message long enough to wrap within the configured body width. ' * 3 +
                           nonce + ' ' + style)
            for size, own_source in (('short', own_short), ('wrapped', own_wrapped)):
                assert len(('PRIVMSG ' + channel + ' :' + own_source).encode()) <= 510
                gui.type_text(own_source)
                gui.framed_keys(w, 'Return')
                ctx['receive']('PRIVMSG', [channel, own_source], ctx['own_nick'])
                ctx['expected_messages'].extend(ctx['channel_messages'](own_source, ctx['own_nick']))
                gui.wait_messages(p, db, ctx['expected_messages'])
                gui._composer_native(w, '', 0, 0)
                captures.append(str(_policy_capture(ctx, 'reply-' + style + '-self-' + size)))
                if size == 'short':
                    # A real peer boundary gives both own bubbles their own
                    # author header; grouping must not hide the wrapped case.
                    _policy_send(ctx, 'Peer author boundary ' + nonce + ' ' + style, target=channel)
            source = ('\x02Formatted quote\x02 café e\u0301 中 \x0304red\x0f ' +
                      'A long retained excerpt wraps across the native two-line quote card. ' * 3 +
                      nonce + ' ' + style)
            _policy_send(ctx, source, target=channel)
            with sqlite3.connect(f'file:{db}?mode=ro', uri=True) as database:
                source_id, = database.execute(
                    'SELECT id FROM messages WHERE buffer=? AND nick=? AND text=?',
                    (identity, ctx['query_target'], source)).fetchone()
            assert source_id and not source_id.startswith('local:'), source_id
            gui.wait_read_marker(p, db, identity)
            draft = 'Keep café \x02raw bold\x02 ' + nonce + ' ' + style + ' 猫🙂'
            _refresh_clipboard(ctx, draft.encode('utf-8'), 'text/plain;charset=utf-8')
            gui.framed_keys(w, 'ctrl+v')
            end = len(draft.encode('utf-8'))
            gui._composer_native(w, draft, end, end)
            gui.framed_keys(w, 'shift+Left')
            cursor = end - len(draft[-1].encode('utf-8'))
            selected_draft = gui._composer_native(w, draft, cursor, end)
            for chord, label in (('ctrl+p', 'selected-other'),
                                 ('ctrl+p', 'selected-own'),
                                 ('ctrl+n', 'selected-other-again')):
                gui.framed_keys(w, chord)
                gui.SESSION.until('actual keyboard transcript selection ' + label,
                                  lambda: gui.native_input_state(w) is None)
                captures.append(str(_policy_capture(ctx, 'reply-' + style + '-' + label)))
            gui.framed_keys(w, 'ctrl+r')
            quoted = gui._composer_native(w, draft, cursor, end)
            captures.append(str(_policy_capture(ctx, 'reply-' + style + '-composer-card')))
            cancellation = gui.cancel_native_reply(w, Path(ctx['destination']),
                label='refinement-reply-' + style + '-cancel', expected_text=draft)
            cancelled = gui._composer_native(w, draft, cursor, end)
            # Cancel restores Input focus: select the newest source again, then
            # send this real reply and check its IRC tag and retained linkage.
            gui.framed_keys(w, 'ctrl+p', 'ctrl+r')
            gui._composer_native(w, draft, cursor, end)
            gui.framed_keys(w, 'Return')
            ctx['receive']('PRIVMSG', [channel, draft], ctx['own_nick'])
            reply_frame = ctx['observer'].frames[-1]
            assert reply_frame['tags'].get('+reply') == source_id, reply_frame
            ctx['expected_messages'].extend(ctx['channel_messages'](draft, ctx['own_nick']))
            gui.wait_messages(p, db, ctx['expected_messages'])
            gui.wait_rows(p, db,
                'SELECT reply,pending,local_echo FROM messages '
                'WHERE buffer=? AND nick=? AND text=? AND own=1',
                (identity, ctx['own_nick'], draft), [(source_id, 0, 0)])
            gui._composer_native(w, '', 0, 0)
            captures.append(str(_policy_capture(ctx, 'reply-' + style + '-in-message-card')))
            gui.framed_keys(w, 'ctrl+p')
            gui.SESSION.until('actual selected sent reply', lambda: gui.native_input_state(w) is None)
            captures.append(str(_policy_capture(ctx, 'reply-' + style + '-selected-own-quote')))
            upload.focus_composer(w)
            gui._composer_native(w, '', 0, 0)
            reply_evidence.append({'style': style, 'source_id': source_id, 'source_raw': source,
                'draft_before': selected_draft, 'quoted_draft': quoted,
                'cancelled_draft': cancelled, 'cancel_control': cancellation,
                'reply_wire': reply_frame, 'captures': captures})
        evidence['reply_cards_and_selected_outlines'] = reply_evidence
        evidence['limitations'].append('Inspect quote-card rasters for rounded outline, nick rail, '
            'bold author and formatted two-line excerpt in composer and message; inspect selected '
            'own/other bubble and IRC outlines against their actual message bounds, excluding avatars. '
            'Inspect short and wrapped self bubbles: own nick right-aligned within the actual bubble; '
            'peer nick remains left-aligned, and IRC author placement is unchanged. '
            'No source, font, hierarchy or fixed-pixel border geometry is pinned.')

    original_fools = original['fools']
    _policy_chat(ctx, fools=original_fools['names'] if isinstance(original_fools, dict) else original_fools)
    gui.set_navigation(p, w, prefs, db, original['workspace']['native_navigation_mode'],
                       'refinement-transcript-restore')
    gui.set_people(p, w, prefs, db, original['workspace']['people_mode'])
    return _refresh_return(ctx, 'transcript-refinements', evidence)



def run_topic_refinements(ctx):
    """Real privileged IRC TOPIC, native header click, exact copy, live/empty state."""
    import sqlite3
    from pathlib import Path

    p, w = ctx['proc'], ctx['window']
    prefs, db = ctx['preferences'], ctx['database_path']
    channel, identity = ctx['channel'], ctx['channel_id']
    observer, nick = ctx['observer'], ctx['query_target']
    destination = Path(ctx['destination'])
    _policy_focus(ctx, identity)
    original_size = gui.window_geometry(w)
    size = original_size['WIDTH'], original_size['HEIGHT']
    initial = gui.save_state(p, prefs, db)
    navigation = initial[0]['native_navigation_mode']
    evidence = []

    def history():
        with sqlite3.connect(f'file:{db}?mode=ro', uri=True) as database:
            return database.execute(
                'SELECT * FROM messages WHERE buffer=? ORDER BY seq',
                (identity,)).fetchall()

    def topic_wire(value):
        line = f'TOPIC {channel} :{value}'
        assert len(line.encode('utf-8')) <= 510, 'Topic exceeds the real IRC framing limit'
        observer.send(line)
        return gui.receive_until(observer, 'TOPIC', [channel, value], nick,
                                 redact=ctx['lab']._redact)

    def selected(value, label):
        gui.SESSION.until('actual focused native topic follows server update',
                          lambda: (gui.native_input_state(w) or {}).get('text') == value)
        marker = gui.GUI_DEBUG[w][1].stat().st_size
        gui.framed_keys(w, 'ctrl+Home', 'ctrl+shift+End')
        state = gui.observe_native_input(w, destination, label, value,
                                        len(value.encode('utf-8')), 0,
                                        after_action=marker)
        sentinel = 'private-topic-copy-sentinel-' + label
        gui.seed_clipboard(sentinel, ctx['cleanup'])
        gui.framed_keys(w, 'ctrl+c')
        gui.SESSION.until('native topic clipboard exact UTF-8 bytes',
                          lambda: gui.clipboard_text() == value)
        copied = gui.clipboard_text()
        assert copied.encode('utf-8') == value.encode('utf-8'), (label, copied, value)
        return {'native': state, 'clipboard_utf8': copied,
                'capture': str(destination.with_name(destination.stem + '-' + label + '.png'))}

    def open_topic():
        layout = gui.read_layout(prefs)
        assert layout['channels']['mode'] == 'expanded', layout
        gui.click(w, *gui.title_actions(layout, layout['scale'])['topic'])

    try:
        # gui_smoke's genuine observer JOIN precedes launch_gui and creates this
        # private channel. Verify the actual server's privileged membership rather
        # than weakening +t or assuming an anonymous late joiner may set TOPIC.
        observer.send('NAMES ' + channel)
        names = []
        while True:
            command, params, prefix = observer.wait(
                lambda command, params, prefix:
                    command in ('353', '366') and channel in params)
            if command == '366':
                break
            names.extend(params[-1].split())
        privileged = any(name.lstrip('~&@%+') == nick and
                         name[:1] in ('~', '&', '@') for name in names)
        assert privileged, ('Private bootstrap observer lacks topic privilege', nick, names)

        # Expanded navigation is the native deterministic wide-header branch;
        # choose enough real viewport space even for the supported maximum zoom.
        zoom = ctx['args'].zoom
        gui.resize_panel_viewport(w, (max(size[0], int(1440 * zoom)),
                                      max(size[1], int(940 * zoom))))
        gui.set_navigation(p, w, prefs, db, 'expanded', 'topic-wide-expanded')
        before = history()
        first = ('Private topic: raw \x02bold\x0f; café 猫 🙂; '
                 'https://example.invalid/topic?a=1&b=2 — ' +
                 'select every wrapped word, not only the visible first line. ' * 4)
        wire = topic_wire(first)
        open_topic()
        evidence.append({'phase': 'header-open-and-exact-copy', 'wire': wire,
                         **selected(first, 'topic-refinements-first')})

        # An already-open native Input must follow the actual wire update without
        # reopening the popover, stale snapshots, or injected model/transcript data.
        second = 'Live replacement: 雪 e\u0301 🙂; ' + 'retained native selection and full raw topic. ' * 5
        wire = topic_wire(second)
        evidence.append({'phase': 'live-topic-without-reshow', 'wire': wire,
                         **selected(second, 'topic-refinements-live')})
        gui.type_text('REJECTED TOPIC EDIT')
        evidence.append({'phase': 'readonly-native-edit-rejected',
                         **selected(second, 'topic-refinements-readonly')})

        wire = topic_wire('')
        # The empty Input's placeholder is raster evidence for parent review;
        # native surrounding text must be truly empty, not fabricated placeholder.
        gui.framed_keys(w, 'ctrl+Home')
        empty = gui.observe_native_input(w, destination, 'topic-refinements-empty', '', 0, 0)
        evidence.append({'phase': 'live-empty-topic', 'wire': wire, 'native': empty,
                         'capture': str(destination.with_name(
                             destination.stem + '-topic-refinements-empty.png')),
                         'visible_placeholder_for_raster_review': 'No topic set'})
        gui.framed_keys(w, 'Escape')
        gui.SESSION.until('Escape leaves topic native Input',
                          lambda: gui.native_input_state(w) is None)
        evidence.append({'phase': 'escape-leaves-topic-input',
                         'capture': str(gui.capture_window(
                             w, destination, 'topic-refinements-dismissed'))})
        # Activate the restored header control instead of inferring focus from
        # text-input disable alone. This also reopens the current empty topic.
        gui.framed_keys(w, 'Return')
        gui.observe_native_input(w, destination, 'topic-refinements-empty-reopen', '', 0, 0)
        gui.framed_keys(w, 'Escape')
        gui.SESSION.until('reopened topic Escape leaves native Input',
                          lambda: gui.native_input_state(w) is None)
        gui.framed_keys(w, 'Return')
        gui.observe_native_input(w, destination, 'topic-refinements-focus-reopen', '', 0, 0)
        gui.framed_keys(w, 'Escape')
        gui.SESSION.until('final topic Escape leaves native Input',
                          lambda: gui.native_input_state(w) is None)
        upload.focus_composer(w)
        gui._composer_native(w, '', 0, 0)
        gui.save_state(p, prefs, db)
        assert history() == before, 'TOPIC synthesized or rewrote retained transcript rows'
        evidence.append({'phase': 'header-only-history-preserved',
                         'unchanged_channel_message_rows': len(before),
                         'bootstrap_topic_privilege_observed': True})
    finally:
        gui.resize_panel_viewport(w, size)
        gui.set_navigation(p, w, prefs, db, navigation, 'topic-restore-navigation')
    return _refresh_return(ctx, 'topic-refinements', evidence)


def run_nick_popup(ctx):
    """Real single-peer completion popup, raw byte edits, and no IRC send."""
    import os

    p, w, prefs, db = ctx['proc'], ctx['window'], ctx['preferences'], ctx['database_path']
    _policy_focus(ctx, ctx['channel_id'])
    _policy_effects(ctx, fx='off')
    upload.focus_composer(w)
    gui.clear_draft()
    gui._composer_native(w, '', 0, 0)
    nick = 'np' + os.urandom(4).hex()
    prefix = nick[:-1]
    captures = []
    observer = ctx['observer']
    with sqlite3.connect(f'file:{db}?mode=ro', uri=True) as database:
        own_before = database.execute(
            'SELECT buffer,id,text FROM messages WHERE own=1 ORDER BY buffer,seq').fetchall()
    wire_start = len(observer.frames)
    with gui.fixtures.BootstrapClient(
            dict(ctx['lab'].endpoints['direct'], nick=nick, username=nick),
            ctx['lab'].ca_file, authenticate=False) as guest:
        guest.send('JOIN ' + ctx['channel'])
        gui.receive_until(observer, 'JOIN', [ctx['channel']], nick, redact=ctx['lab']._redact)
        gui.wait_rows(p, db,
            'SELECT nick,activity FROM messages WHERE buffer=? AND nick=? AND presence=1 ORDER BY seq',
            (ctx['channel_id'], nick), [(nick, 1)])
        gui.type_text(prefix)
        gui.framed_keys(w, 'Left', 'Right')
        before = gui._composer_native(w, prefix, len(prefix), len(prefix))
        image = gui.native_rgb_capture(w)
        x, y, width, _ = gui.read_layout(prefs)['conversation']['rect']
        region = (round(x), round(y), round(x + width), round(before['caret'][1] - 8))
        baseline = _policy_crop(image, region)
        gui.framed_keys(w, 'Tab')
        unchanged = gui._composer_native(w, prefix, len(prefix), len(prefix))
        painted = _policy_crop(gui.native_rgb_capture(w), region)
        size = (region[2] - region[0], region[3] - region[1])
        difference = ImageChops.difference(Image.frombytes('RGB', size, baseline),
                                           Image.frombytes('RGB', size, painted)).getbbox()
        assert difference and difference[2] - difference[0] > before['caret'][2], \
            'Explicit completion did not paint a popup above the unchanged single-match token'
        captures.append(str(_policy_capture(ctx, 'nick-popup-single-candidate')))
        gui.framed_keys(w, 'Down', 'Up')
        gui._composer_native(w, prefix, len(prefix), len(prefix))
        gui.framed_keys(w, 'Return')
        accepted = nick + ': '
        after = gui._composer_native(w, accepted, len(accepted), len(accepted))
        captures.append(str(_policy_capture(ctx, 'nick-popup-enter-accepted-not-sent')))
        gui.command('undo', window=w)
        undone = gui._composer_native(w, prefix, len(prefix), len(prefix))
        gui.framed_keys(w, 'Tab', 'Escape')
        escaped = gui._composer_native(w, prefix, len(prefix), len(prefix))
        gui.clear_draft()
        suffix = ' \x02café\x02 e\u0301 中'
        raw = 'say ' + prefix + suffix
        _refresh_clipboard(ctx, raw.encode('utf-8'), 'text/plain;charset=utf-8')
        gui.framed_keys(w, 'ctrl+v')
        gui._composer_native(w, raw, len(raw.encode()), len(raw.encode()))
        caret = len('say ' + prefix)
        gui.framed_keys(w, 'ctrl+Home', *(['Right'] * caret))
        gui._composer_native(w, raw, caret, caret)
        gui.framed_keys(w, 'Tab')
        gui._composer_native(w, raw, caret, caret)
        captures.append(str(_policy_capture(ctx, 'nick-popup-retained-raw-suffix')))
        gui.framed_keys(w, 'Tab')
        completed_raw = 'say ' + nick + suffix
        caret_after = len('say ' + nick)
        suffix_state = gui._composer_native(w, completed_raw, caret_after, caret_after)
        gui.command('undo', window=w)
        suffix_undo = gui._composer_native(w, raw, caret, caret)
        gui.clear_draft()
        unmatched = 'notanick' + os.urandom(6).hex()
        gui.type_text(unmatched)
        gui.framed_keys(w, 'Tab')
        no_match = gui._composer_native(w, unmatched, len(unmatched), len(unmatched))
        # A positive server barrier plus retained own-message rows supplements
        # the nonempty native draft witness after accepting with Enter.
        token = 'nick-popup-barrier-' + str(time.monotonic_ns())
        observer.send('PING :' + token)
        observer.wait(lambda command, params, prefix:
            command == 'PONG' and params and params[-1] == token, timeout=15)
        gui.save_state(p, prefs, db)
        sent = [frame for frame in observer.frames[wire_start:]
                if frame['command'] == 'PRIVMSG' and frame['prefix'].split('!', 1)[0]
                in {ctx['own_nick'], ctx['other_nick']}]
        assert not sent, ('Nick completion sent a message', sent)
        with sqlite3.connect(f'file:{db}?mode=ro', uri=True) as database:
            own_after = database.execute(
                'SELECT buffer,id,text FROM messages WHERE own=1 ORDER BY buffer,seq').fetchall()
        assert own_after == own_before, 'Nick popup acceptance created an own chat message'
        upload.focus_composer(w)
        gui.clear_draft()
        gui._composer_native(w, '', 0, 0)
        return _refresh_return(ctx, 'nick-popup', {
            'single_candidate_prefix': prefix, 'actual_guest_nick': nick,
            'unchanged_while_open': unchanged, 'paint_difference_bbox': difference,
            'accepted': after, 'one_undo': undone, 'escape_preserved': escaped,
            'retained_raw_suffix': suffix_state, 'suffix_one_undo': suffix_undo,
            'nonnick_unchanged': no_match, 'no_own_privmsg_on_wire': True,
            'no_own_message_rows': True, 'captures': captures})


def run_whois_profile(ctx):
    """Real WHOIS profile fields/copy, slash and message routes, error and no spam."""
    import os

    p, w, prefs, db = ctx['proc'], ctx['window'], ctx['preferences'], ctx['database_path']
    observer, nick = ctx['observer'], ctx['query_target']
    destination = Path(ctx['destination'])
    _policy_focus(ctx, ctx['channel_id'])
    _policy_effects(ctx, fx='off')
    gui.set_navigation(p, w, prefs, db, 'hidden', 'profile-consumer')
    gui.set_people(p, w, prefs, db, 'hidden')
    observer.send('WHOIS ' + nick)
    _, identity, server = observer.wait(lambda command, params, prefix:
        command == '311' and len(params) >= 6 and params[1] == nick, timeout=15)
    _, terminal, _ = observer.wait(lambda command, params, prefix:
        command == '318' and len(params) >= 2 and params[1] == nick, timeout=15)
    expected = {'Nick': identity[1], 'Username': identity[2],
                'Host': identity[3], 'Real name': identity[5]}
    assert all(expected.values()), identity
    nonce = os.urandom(4).hex()
    marker = (71, 174, 211)
    source = '\x0447AED3,47AED3PROFILE\x0f source ' + nonce
    _policy_send(ctx, source, target=ctx['channel'])
    gui.wait_read_marker(p, db, ctx['channel_id'])
    captures, evidence = [], []

    def capture(label):
        path = gui.capture_window(w, destination, 'whois-profile-' + label)
        captures.append(str(path))
        return str(path)

    def history():
        with sqlite3.connect(f'file:{db}?mode=ro', uri=True) as database:
            return database.execute('SELECT * FROM messages ORDER BY seq').fetchall()

    def field(caption, value):
        def focused():
            state = gui.native_input_state(w)
            return state if state and state['caret'] and state['text'] == value else False
        return gui.SESSION.until('actual selectable profile value ' + caption, focused)

    def seek_field(label, matches):
        # Opening focuses Close. Native Tab also scrolls offscreen inputs into view.
        def advance():
            gui.framed_keys(w, 'Tab')
            state = gui.native_input_state(w)
            return state if state and state['caret'] and matches(state['text'] or '') else False
        return gui.SESSION.until('native profile focus reaches ' + label, advance)

    def copy_field(caption, value):
        field(caption, value)
        gui.framed_keys(w, 'ctrl+Home', 'ctrl+shift+End')
        end = len(value.encode('utf-8'))
        selected = gui._composer_native(w, value, end, 0)
        gui.seed_clipboard('private-profile-field-sentinel-' + caption, ctx['cleanup'])
        gui.framed_keys(w, 'ctrl+c')
        gui.SESSION.until('exact native profile field clipboard bytes',
                          lambda: gui.clipboard_text() == value)
        return {'native': selected, 'clipboard': gui.clipboard_text()}

    def ready(label):
        initial_capture = capture(label + '-initial')
        seek_field('wire nick', lambda text: text == expected['Nick'])
        values = {}
        for index, (caption, value) in enumerate(expected.items()):
            if index:
                gui.framed_keys(w, 'Tab')
            values[caption] = copy_field(caption, value)
        # First prove every 311 value; then verify Loading/Error have no tab stop
        # between the first identity input and the header Close control.
        gui.framed_keys(w, *(['shift+Tab'] * len(expected)))
        gui.SESSION.until('completed profile returns directly to non-text header focus',
                          lambda: gui.native_input_state(w) is None)
        gui.framed_keys(w, *(['Tab'] * len(expected)), 'ctrl+Home', 'ctrl+shift+End')
        realname = expected['Real name']
        gui._composer_native(w, realname, len(realname.encode('utf-8')), 0)
        capture(label + '-identity-and-generated-avatar')
        return {'fields': values, 'initial_capture': initial_capture}

    def close(text, cursor, anchor):
        gui.framed_keys(w, 'Escape')
        return gui._composer_native(w, text, cursor, anchor)

    before = history()
    command = '/whois ' + nick
    gui.type_text(command)
    gui.framed_keys(w, 'Return')
    slash = ready('slash')
    # Native fields must be selectable without becoming editable.
    field('Real name', expected['Real name'])
    gui.framed_keys(w, 'ctrl+Home', 'ctrl+shift+End')
    gui.type_text('REJECTED PROFILE EDIT')
    assert (gui.native_input_state(w) or {}).get('text') == expected['Real name']
    slash['closed_composer'] = close('', 0, 0)
    assert gui.save_state(p, prefs, db)[1] == (ctx['channel_id'],)
    assert history() == before, 'Slash WHOIS added or rewrote retained buffer messages'
    evidence.append({'route': 'slash', **slash})

    draft = 'Keep \x02café\x02 e\u0301 ' + nonce + ' 猫🙂'
    _refresh_clipboard(ctx, draft.encode('utf-8'), 'text/plain;charset=utf-8')
    gui.framed_keys(w, 'ctrl+v')
    end = len(draft.encode('utf-8'))
    gui._composer_native(w, draft, end, end)
    gui.framed_keys(w, 'shift+Left')
    cursor = end - len('🙂'.encode('utf-8'))
    draft_before = gui._composer_native(w, draft, cursor, end)
    source_box = _policy_wait_box(ctx, marker)
    popup = gui.open_native_popup(w, lambda: (
        gui.move_pointer(w, (source_box[0] + source_box[2]) / 2,
                         (source_box[1] + source_box[3]) / 2),
        gui.POINTER.button(True, code=273), gui.POINTER.button(False, code=273)))
    # A peer message has no Delete entry: its final action is Whois, after any
    # capability-dependent Reply/React actions. No technical-nick OCR locator.
    gui.popup_keys(w, popup, 'End', 'Return')
    menu = ready('message-action')
    menu['draft_before'] = draft_before
    assert history() == before, 'Message Whois added or rewrote retained buffer messages'
    with sqlite3.connect(f'file:{db}?mode=ro', uri=True) as database:
        frontier = database.execute('SELECT COALESCE(MAX(seq),0) FROM messages').fetchone()[0]
    ordinary = 'Ordinary message while profile is open café ' + nonce
    _policy_send(ctx, ordinary, target=ctx['channel'])
    with sqlite3.connect(f'file:{db}?mode=ro', uri=True) as database:
        old_rows = database.execute(
            'SELECT * FROM messages WHERE seq<=? ORDER BY seq', (frontier,)).fetchall()
        arrivals = database.execute(
            'SELECT buffer,nick,text,own,pending,presence,activity,deleted FROM messages '
            'WHERE seq>? ORDER BY buffer', (frontier,)).fetchall()
    assert old_rows == before, 'Profile swallowed normal traffic or rewrote existing history'
    expected_arrivals = [(*row, 0, 0, 0) for row in
                         ctx['channel_messages'](ordinary, ctx['query_target'])]
    assert sorted(arrivals) == sorted(expected_arrivals), (arrivals, expected_arrivals)
    realname = expected['Real name']
    menu['ordinary_arrival_profile_selection'] = gui._composer_native(
        w, realname, len(realname.encode('utf-8')), 0)
    menu['ordinary_arrival_rows'] = arrivals
    capture('ordinary-traffic-profile-focus-retained')
    before = history()
    menu['closed_composer'] = close(draft, cursor, end)
    gui.save_state(p, prefs, db)
    assert history() == before, 'Message Whois added or rewrote retained buffer messages'
    evidence.append({'route': 'message-context', **menu})

    # A real unknown target exercises honest server failure, not an injected
    # transport error. The slash command itself is consumed like any command.
    gui.clear_draft()
    missing = 'missing' + os.urandom(5).hex()
    observer.send('WHOIS ' + missing)
    _, failure, failure_server = observer.wait(lambda command, params, prefix:
        command == '401' and len(params) >= 3 and params[1] == missing, timeout=15)
    gui.type_text('/whois ' + missing)
    gui.framed_keys(w, 'Return')
    capture('error-initial')
    error = seek_field('wire failure', lambda text: bool(text) and failure[-1] in text)
    assert error['text'] and failure[-1] in error['text'], (failure, error)
    copied_error = copy_field('Error', error['text'])
    capture('real-unknown-nick-error')
    error_closed = close('', 0, 0)
    gui.save_state(p, prefs, db)
    assert history() == before, 'Failed WHOIS added or rewrote retained buffer messages'
    evidence.append({'route': 'real-401', 'wire': failure, 'server': failure_server,
                     'copied_error': copied_error, 'closed_composer': error_closed})
    return _refresh_return(ctx, 'whois-profile', {
        'wire_311': {'params': identity, 'server': server}, 'wire_318': terminal,
        'evidence': evidence, 'captures': captures, 'whois_added_or_rewrote_no_buffer_messages': True,
        'limitations': ['Inspect actual profile hierarchy and generated initials/avatar rasters; '
            'no remote avatar photo is asserted.',
            'Initial captures show actual scheduling; the fast private daemon may finish before '
            'the loading frame is captured. No artificial delay or source-state hook is used.']})


def run_messaging_refinements(ctx):
    """Real source policies, global read-only feed and nonmoving mention controls."""
    p, w = ctx['proc'], ctx['window']
    prefs, db = ctx['preferences'], ctx['database_path']
    channel, query = ctx['channel_id'], ctx['query_id']
    _policy_focus(ctx, query)
    _policy_effects(ctx, fx='off')
    _policy_chat(ctx, fools=[], receive=False)
    gui.set_message_style(p, w, prefs, db, ctx['args'], ctx['destination'],
                          query, 'irc', label='messaging-query-irc')
    _policy_focus(ctx, channel)
    gui.set_message_style(p, w, prefs, db, ctx['args'], ctx['destination'],
                          channel, 'irc', label='messaging-channel-irc')
    evidence, captures = [], []
    colors = ((45, 155, 217), (217, 69, 119), (42, 185, 113), (217, 145, 42))
    bodies = [
        '\x042D9BD9,2D9BD9CHANNELONE\x0f ' + ctx['own_nick'] + ': FirstSourceMention',
        '\x04D94577,D94577QUERYTWO\x0f ' + ctx['own_nick'] + ': SecondSourceMention',
        '\x042AB971,2AB971CHANNELTHREE\x0f ' + ctx['own_nick'] + ': ThirdSourceMention',
        '\x04D9912A,D9912AOTHERNETWORK\x0f ' + ctx['other_nick'] + ': FourthSourceMention',
    ]

    def retained():
        with sqlite3.connect(f'file:{db}?mode=ro', uri=True) as connection:
            return connection.execute(
                'SELECT buffer,id,nick,text,mention,own FROM messages WHERE text IN (?,?,?,?) '
                'ORDER BY seq', bodies).fetchall()

    def words(label):
        x, y, width, height = gui.read_layout(prefs)['conversation']['rect']
        # Physically crop/upscale the native conversation before recognizing
        # highlighted mention rows; full-window PSM11 classified them as blocks.
        rows = upload.visible_words(w, Path(ctx['destination']), label,
                                    region=(x, y, width, height), psm=6)
        return [(row['text'].strip(), (int(row['left']), int(row['top']),
                 int(row['left']) + int(row['width']), int(row['top']) + int(row['height']))) for row in rows]

    def marker(index, present=True):
        def observed():
            box = _policy_box(ctx, colors[index])
            return box if present else not box
        return gui.SESSION.until('actual source marker ' + str(index) + ' ' + str(present), observed)

    def activate_feed():
        gui.command('global-mentions', window=w)
        state = gui.save_state(p, prefs, db)
        assert len(state[1]) == 1 and state[1][0] not in (channel, query), (
            'Global Mentions did not open its own read-only buffer', state)
        return state[1][0]

    def policy(command, identity):
        expected = channel_edit if identity == channel else ('', 0, 0)
        _policy_focus(ctx, identity, expected=expected)
        gui.command(command, window=w)
        gui._composer_native(w, *expected)
        gui.save_state(p, prefs, db)

    # A real nonmention body anchors the viewport before the floating control
    # first exists. No preloaded UI/model state is injected.
    anchor_color = (179, 79, 213)
    _policy_send(ctx, '\x04B34FD5,B34FD5STABLEFOOTER\x0f', target=ctx['channel'])
    anchor = _policy_wait_box(ctx, anchor_color)
    caret = gui.native_input_rectangle(w)
    layout = gui.read_layout(prefs)['conversation']['rect']
    _policy_send(ctx, bodies[0], target=ctx['channel'])
    first = marker(0)
    assert _policy_wait_box(ctx, anchor_color) == anchor, 'First mention control shifted existing history'
    assert gui.native_input_rectangle(w) == caret, 'Mention control moved composer with typing disabled'
    assert gui.read_layout(prefs)['conversation']['rect'] == layout
    _policy_send(ctx, bodies[1])
    _policy_send(ctx, bodies[2], target=ctx['channel'])
    _policy_send(ctx, bodies[3], target=ctx['channel'])
    marker(2)
    retained_rows = retained()
    source_rows = [row for row in retained_rows if row[4]]
    assert len(retained_rows) == 7 and len(source_rows) == 4, retained_rows
    assert all(row[4:] == (1, 0) for row in source_rows), source_rows
    assert {(row[0], row[3]) for row in source_rows} == {
        (channel, bodies[0]), (query, bodies[1]), (channel, bodies[2]),
        (ctx['other_channel_id'], bodies[3])}, source_rows
    duplicate_source_ids = []
    for body in (bodies[0], bodies[2], bodies[3]):
        pair = [row for row in retained_rows if row[3] == body]
        assert len(pair) == 2 and pair[0][0] != pair[1][0], pair
        if pair[0][1] == pair[1][1]:
            duplicate_source_ids.append([(row[0], row[1]) for row in pair])
    captures.append(str(_policy_capture(ctx, 'messaging-four-sources')))

    # Exclude a genuine outgoing whole-nick mention rather than a fake own flag.
    own_body = ctx['own_nick'] + ': OwnMentionMustNotAppear'
    gui.type_text(own_body)
    gui.framed_keys(w, 'Return')
    ctx['receive']('PRIVMSG', [ctx['channel'], own_body], ctx['own_nick'])
    ctx['expected_messages'].extend(ctx['channel_messages'](own_body, ctx['own_nick']))
    gui.wait_messages(p, db, ctx['expected_messages'])
    draft = 'source draft café e\u0301 猫🙂'
    gui.type_text(draft)
    byte_end = len(draft.encode('utf-8'))
    draft_state = gui._composer_native(w, draft, byte_end, byte_end)
    channel_edit = (draft_state['text'], draft_state['cursor'], draft_state['anchor'])
    feed = activate_feed()
    boxes = [marker(index) for index in range(4)]
    assert all(newer[3] < older[1] for newer, older in zip(boxes[:0:-1], boxes[-2::-1])), (
        'Global Mentions is not newest-first across actual source buffers', boxes)
    rendered = words('messaging-feed-newest-first')
    assert not any('ownmentionmustnotappear' in word.casefold() for word, _ in rendered), rendered
    # Feed text must carry the original source context, not just an anonymous
    # synthetic message. Query sender and channel are native consumer labels.
    context_text = ' '.join(word for word, _ in rendered).casefold()
    assert ctx['channel'].lstrip('#').casefold() in context_text, rendered
    assert ctx['query_target'].casefold() in context_text, rendered
    before_feed = gui.save_state(p, prefs, db)
    readonly_offset = gui.GUI_DEBUG[w][1].stat().st_size
    gui.type_text('MustNotSendFromGlobalFeed')
    gui.framed_keys(w, 'Return')
    # With no composer, Return may activate a focused toolbar/menu control.
    # Dismiss its real native popup before asking the palette to save state.
    readonly_trace = gui.GUI_DEBUG[w][1].read_bytes()[readonly_offset:].decode(errors='replace')
    live_popups = set()
    for event in re.finditer(r'get_popup\(new id xdg_popup[@#](\d+),|xdg_popup[@#](\d+)\.destroy\(',
                             readonly_trace):
        if event[1]:
            live_popups.add(event[1])
        else:
            live_popups.discard(event[2])
    if live_popups:
        popup = gui.await_native_popup(w, readonly_offset)
        captures.append(str(gui.capture_native_popup(w, popup, Path(ctx['destination']),
                                                     'messaging-readonly-overflow')))
        gui.popup_keys(w, popup, *(['Escape'] * len(live_popups)), root_changed=False)
    assert gui.native_input_state(w) is None, 'Read-only Mentions unexpectedly owns native editable input'
    after_feed = gui.save_state(p, prefs, db)
    assert after_feed[1] == before_feed[1] == (feed,), (before_feed, after_feed)
    with sqlite3.connect(f'file:{db}?mode=ro', uri=True) as connection:
        assert not connection.execute(
            'SELECT 1 FROM messages WHERE text LIKE ?', ('%MustNotSendFromGlobalFeed%',)).fetchall()
        draft_rows = connection.execute('SELECT draft FROM buffers WHERE id=?', (channel,)).fetchall()
    assert draft_rows == [(draft,)], ('Read-only feed damaged original draft', draft_rows)
    captures.append(str(_policy_capture(ctx, 'messaging-readonly-feed')))

    # One author is a fool in both sources. Opposite explicit policies must
    # resolve per original source, never from the currently active aggregate.
    _policy_focus(ctx, channel, expected=channel_edit)
    _policy_chat(ctx, fools=[ctx['query_target']])
    _policy_appearance(ctx, fool='hidden')
    policy('show-fools', channel)
    marker(0)
    marker(2)
    policy('hide-fools', query)
    marker(1, False)
    activate_feed()
    marker(0)
    marker(2)
    marker(1, False)
    evidence.append({'phase': 'opposite-source-policies', 'shown': channel, 'hidden': query,
                     'feed': feed, 'source_rows': source_rows})
    captures.append(str(_policy_capture(ctx, 'messaging-feed-source-policy')))

    # Explicit Show is sharp/readable even while global Hidden remains in force.
    policy('show-fools', query)
    query_box = marker(1)
    popup = gui.open_native_popup(w, lambda: (
        gui.move_pointer(w, query_box[2] + 12, (query_box[1] + query_box[3]) / 2),
        gui.POINTER.button(True, code=273), gui.POINTER.button(False, code=273)))
    gui.popup_keys(w, popup, 'Home', 'Return', root_changed=False)
    decoded = 'QUERYTWO ' + ctx['own_nick'] + ': SecondSourceMention'
    gui.SESSION.until('explicit Show copies full original fool body', lambda: gui.clipboard_text() == decoded)
    # Build an actual reply before revoking its source readability.
    popup = gui.open_native_popup(w, lambda: (
        gui.move_pointer(w, query_box[2] + 12, (query_box[1] + query_box[3]) / 2),
        gui.POINTER.button(True, code=273), gui.POINTER.button(False, code=273)))
    gui.popup_keys(w, popup, 'Home', 'Down', 'Return')
    gui._composer_native(w, '', 0, 0)
    preview_region = _policy_reply_region(ctx, 'messaging-reply-shown')
    shown_preview = _policy_words(ctx, 'messaging-reply-shown', preview_region)
    assert any('secondsourcemention' in word.casefold() for word, _ in shown_preview), shown_preview
    gui.command('hide-fools', window=w)
    marker(1, False)
    hidden_preview = _policy_words(ctx, 'messaging-reply-hidden',
                                   _policy_reply_region(ctx, 'messaging-reply-hidden'))
    assert not any('secondsourcemention' in word.casefold() for word, _ in hidden_preview), hidden_preview
    gui.cancel_native_reply(w, ctx['destination'], label='messaging-hidden-reply-cancel', expected_text='')
    # Default chord is a real consumer too; the first toggle from explicit Hide
    # must become full Show without resetting the global preference.
    gui.framed_keys(w, 'ctrl+x', 'f')
    marker(1)
    gui.save_state(p, prefs, db)
    saved = tomllib.loads(Path(prefs).read_text())
    assert saved['appearance']['fool_display'] == 'hidden', saved['appearance']
    activate_feed()
    for index in range(3):
        marker(index)
    # A first native body click jumps directly; no prior row selection or
    # incidental network-label spelling is part of this consumer contract.
    query_marker = marker(1)
    gui.click(w, (query_marker[0] + query_marker[2]) / 2,
              (query_marker[1] + query_marker[3]) / 2)
    source_state = gui.save_state(p, prefs, db)
    assert source_state[1] == (query,), ('Source context jumped to wrong buffer', source_state)
    marker(1)
    gui._composer_native(w, '', 0, 0)
    evidence.append({'phase': 'context-source-jump', 'selected': list(source_state[1]),
                     'original': [(row[0], row[1]) for row in source_rows if row[0] == query]})

    evidence.append({'phase': 'explicit-show-copy-and-hidden-reply',
                     'clipboard': decoded, 'shown_preview': shown_preview,
                     'hidden_preview': hidden_preview, 'global_style_unchanged': 'hidden'})
    # Both configured private network connections receive this exact wire
    # message. If Ergo preserves its msgid across them, this exercises a real
    # composite-ID collision in the global feed without forged server tags.
    policy('show-fools', ctx['other_channel_id'])
    shared_body = '\x04A078DC,A078DCSHAREDSOURCE\x0f ' + ctx['own_nick'] + ' ' + ctx['other_nick']
    _policy_send(ctx, shared_body, target=ctx['channel'])
    with sqlite3.connect(f'file:{db}?mode=ro', uri=True) as connection:
        shared_rows = connection.execute(
            'SELECT buffer,id,mention FROM messages WHERE text=? ORDER BY buffer', (shared_body,)).fetchall()
    assert len(shared_rows) == 2 and all(row[2] == 1 for row in shared_rows), shared_rows
    activate_feed()
    def shared_visible():
        image = gui.native_rgb_capture(w)
        region = gui.read_layout(prefs)['conversation']['rect']
        components = gui.native_marker_components(image, region, (160, 120, 220))
        return components if len(components) == 2 and all(item['full'] for item in components) else False
    shared_components = gui.SESSION.until('both original network copies visible in global feed', shared_visible)
    captures.append(str(_policy_capture(ctx, 'messaging-shared-msgid')))
    evidence.append({'phase': 'same-wire-two-original-sources', 'rows': shared_rows,
                     'server_preserved_same_msgid': shared_rows[0][1] == shared_rows[1][1],
                     'visible_originals': shared_components})
    # Reuse the already-owned real input-method-v2 client and its full native
    # glyph/partial-commit/cancel/clipboard proof after the new source actions.
    _policy_focus(ctx, query)
    ime_text = gui.probe_real_ime(w, Path(ctx['destination']), ctx['cleanup'],
                                  gui.read_layout(prefs), ctx['args'].zoom,
                                  method=ctx['input_method'])
    ime_bytes = len(ime_text.encode('utf-8'))
    gui._composer_native(w, ime_text, ime_bytes, ime_bytes)
    gui.framed_keys(w, 'Return')
    ctx['receive']('PRIVMSG', [ctx['query_target'], ime_text], ctx['own_nick'])
    ctx['expected_messages'].append((query, ctx['own_nick'], ime_text, 1, 0))
    gui.wait_messages(p, db, ctx['expected_messages'])
    evidence.append({'phase': 'real-ime-after-source-actions',
                     'sent': ime_text, 'utf8_bytes': ime_bytes, 'target': query})
    gui.save_state(p, prefs, db)
    with sqlite3.connect(f'file:{db}?mode=ro', uri=True) as connection:
        final_source_draft = connection.execute('SELECT draft FROM buffers WHERE id=?', (channel,)).fetchall()
    assert final_source_draft == [(draft,)], ('Source policy/navigation/IME changed original draft', final_source_draft)
    return _refresh_return(ctx, 'messaging-refinements', {
        'phases': evidence, 'captures': captures, 'readonly_source_draft': draft,
        'first_control_anchor': anchor, 'first_control_caret': caret,
        'first_source_marker': first, 'source_identity': source_rows,
        'mirrored_network_duplicate_ids': duplicate_source_ids,
        'limitations': [
            'Ergo assigns msgid; mirrored private network copies report actual duplicate IDs above, '
            'without forging tags or requesting unsupported capabilities. Exhaustive composite-ID '
            'collisions and >200 retained/backfilled mentions belong to Core regressions.',
            'Typing color/animation/geometry runs in the existing typing scope. Fork IME/focus/clipping/'
            'popup behavior runs through unchanged gui_smoke --probes and nick-popup scopes.']})


def _native_cursor_shape(ctx, point, expected, label):
    """Observe cursor-shape-v1 requests after a genuine private pointer motion."""
    w = ctx['window']
    debug = gui.GUI_DEBUG[w][1]
    offset = debug.stat().st_size
    gui.move_pointer(w, *point)
    result = None

    def observed():
        nonlocal result
        assert ctx['proc'].poll() is None, 'Native cursor consumer exited'
        trace = debug.read_bytes()[offset:].decode(errors='replace')
        shapes = list(re.finditer(r'wp_cursor_shape_device_v1[@#]\d+\.set_shape\((\d+),\s*(\d+)\)', trace))
        if not shapes:
            return False
        result = {'label': label, 'point': point, 'shape': int(shapes[-1][2]),
                  'serial': int(shapes[-1][1]), 'native_request': shapes[-1][0]}
        return result if result['shape'] == expected else False

    # Values are the public cursor-shape-v1 protocol enum, not SDK internals:
    # default=1, pointer=4, text=9. Each caller changes shape before observing.
    gui.SESSION.until('actual Wayland cursor shape ' + label + '=' + str(expected), observed)
    return result


def run_cursor_affordances(ctx):
    """Actual Wayland Arrow/Text/Hand on native canvas, text, link and author."""
    p, w, prefs, db = ctx['proc'], ctx['window'], ctx['preferences'], ctx['database_path']
    _policy_focus(ctx, ctx['query_id'])
    _policy_effects(ctx, fx='off')
    _policy_chat(ctx, fools=[])
    gui.set_message_style(p, w, prefs, db, ctx['args'], ctx['destination'],
                          ctx['query_id'], 'irc', label='cursor-native-irc')
    origin = ctx['cleanup'].enter_context(_RefreshMediaOrigin(ctx['lab'], {
        '/cursor': (b'Private cursor target', 'text/plain')}))
    _policy_send(ctx, '\x042D9BD9,2D9BD9PLAINLEFT\x0f PlainCursorWords \x04D94577,D94577PLAINRIGHT\x0f')
    _policy_send(ctx, '\x042AB971,2AB971LINKLEFT\x0f ' + origin.url +
                 '/cursor \x04B34FD5,B34FD5LINKRIGHT\x0f')
    plain_a, plain_b = _policy_wait_box(ctx, (45, 155, 217)), _policy_wait_box(ctx, (217, 69, 119))
    link_a, link_b = _policy_wait_box(ctx, (42, 185, 113)), _policy_wait_box(ctx, (179, 79, 213))
    assert abs(plain_a[1] - plain_b[1]) <= 1 and abs(link_a[1] - link_b[1]) <= 1, (
        'Cursor fixtures unexpectedly wrapped', plain_a, plain_b, link_a, link_b)
    plain = ((plain_a[2] + plain_b[0]) / 2, (plain_a[1] + plain_a[3]) / 2)
    link = ((link_a[2] + link_b[0]) / 2, (link_a[1] + link_a[3]) / 2)
    layout = gui.read_layout(prefs)
    x, _, width, _ = layout['conversation']['rect']
    metrics = gui.pane_metrics(layout, layout['scale'])
    blank = (x + width - metrics['left'] - 24 * layout['scale'], plain[1])
    assert blank[0] > max(plain_b[2], link_b[2]) + 16, ('No native blank-canvas witness', blank)
    author_words = _policy_words(ctx, 'cursor-author-reference')
    authors = [box for text, box in author_words
               if text.rstrip(':').casefold() == ctx['query_target'].casefold()
               and x <= box[0] and box[2] < plain_a[0] and box[3] <= plain_a[3]]
    assert authors, ('No actual clickable IRC author label', author_words)
    # Consecutive messages share one actual IRC author prefix. Use the
    # nearest preceding visible group label, not a fabricated repeated author.
    author = max(authors, key=lambda box: box[3])
    author_point = ((author[0] + author[2]) / 2, (author[1] + author[3]) / 2)
    composer_before = gui.native_input_state(w)
    requests = [
        _native_cursor_shape(ctx, link, 4, 'actual-link-hand'),
        _native_cursor_shape(ctx, plain, 9, 'ordinary-message-text'),
        _native_cursor_shape(ctx, author_point, 4, 'clickable-author-hand'),
        _native_cursor_shape(ctx, blank, 1, 'empty-canvas-arrow'),
        _native_cursor_shape(ctx, link, 4, 'actual-link-hand-again'),
        _native_cursor_shape(ctx, plain, 9, 'ordinary-message-text-again'),
    ]
    assert gui.native_input_state(w) == composer_before, 'Hover mutated native composer or focus'
    assert not origin.snapshot(), 'Hover fetched private link without consent'
    return _refresh_return(ctx, 'cursor-affordances', {
        'requests': requests, 'plain_markers': [plain_a, plain_b], 'link_markers': [link_a, link_b],
        'clickable_author': author, 'no_hover_fetch': True,
        'capture': str(_policy_capture(ctx, 'cursor-affordances')),
        'protocol': 'cursor-shape-v1 enum default=1,pointer=4,text=9; actual set_shape requests'})


def _native_word_ink(ctx, box, *, padding=2):
    """Measure actual opaque glyph cores and their native plate, not theme roles."""
    from collections import Counter
    image = gui.native_rgb_capture(ctx['window'])
    left, top, right, bottom = map(round, box)
    region = (max(0, left - padding), max(0, top - 1),
              min(image['width'], right + padding), min(image['height'], bottom + 1))
    pixels = _policy_crop(image, region)
    colors = Counter(tuple(pixels[index:index + 3]) for index in range(0, len(pixels), 3))
    plate, plate_count = colors.most_common(1)[0]
    def luminance(rgb):
        linear = [value / 3294.6 if value <= 10 else ((value / 255 + .055) / 1.055) ** 2.4
                  for value in rgb]
        return sum(value * weight for value, weight in zip(linear, (.2126, .7152, .0722)))
    def contrast(rgb):
        a, b = luminance(rgb), luminance(plate)
        return (max(a, b) + .05) / (min(a, b) + .05)
    candidates = [(contrast(rgb), count, rgb) for rgb, count in colors.items()
                  if count >= 3 and max(abs(a - b) for a, b in zip(rgb, plate)) >= 24]
    assert candidates, ('No real glyph cores against native plate', box, colors.most_common(8))
    ratio, count, ink = max(candidates)
    return {'box': region, 'plate': plate, 'ink': ink, 'contrast': ratio,
            'ink_pixels': count, 'plate_pixels': plate_count,
            'core_colors': [rgb for _, _, rgb in candidates]}


def run_composer_affordances(ctx):
    """Known-nick native ink, lossless raw edits/wire, and readable popup states."""
    import contextlib
    p, w, prefs, db = ctx['proc'], ctx['window'], ctx['preferences'], ctx['database_path']
    identity, channel = ctx['channel_id'], ctx['channel']
    _policy_focus(ctx, identity)
    _policy_effects(ctx, fx='off')
    _policy_chat(ctx, fools=[])
    gui.set_message_style(p, w, prefs, db, ctx['args'], ctx['destination'],
                          identity, 'irc', label='composer-affordances-author-reference')
    captures, evidence = [], []
    nicks = ('NativeAmber', 'NativeAzure')

    def author_box(nick, marker):
        from difflib import SequenceMatcher
        words = _policy_words(ctx, 'composer-author-' + nick)
        left = gui.read_layout(prefs)['conversation']['rect'][0]
        # OCR actually read the visible fixture NativeAmber as "vativeamber".
        # Bound the match to its source column/group; identity and ink checks
        # remain the real private peer and native rendered foreground.
        candidates = [box for text, box in words
                      if SequenceMatcher(None, text.rstrip(':').casefold(), nick.casefold()).ratio() >= .85
                      and left <= box[0] and box[2] < marker[0] and box[3] <= marker[3]]
        assert candidates, ('No actual IRC author reference', nick, words)
        return max(candidates, key=lambda box: box[3])

    with contextlib.ExitStack() as peers:
        guests = []
        for nick in nicks:
            guest = peers.enter_context(gui.fixtures.BootstrapClient(
                dict(ctx['lab'].endpoints['direct'], nick=nick, username=nick),
                ctx['lab'].ca_file, authenticate=False))
            guest.send('JOIN ' + channel)
            gui.receive_until(ctx['observer'], 'JOIN', [channel], nick, redact=ctx['lab']._redact)
            gui.wait_rows(p, db,
                'SELECT nick,activity FROM messages WHERE buffer=? AND nick=? AND presence=1 ORDER BY seq',
                (identity, nick), [(nick, 1)])
            guests.append(guest)
        source_body = '\x042D9BD9,2D9BD9GUESTCOLOR\x0f'
        guests[0].send(f'PRIVMSG {channel} :{source_body}')
        gui.receive_until(ctx['observer'], 'PRIVMSG', [channel, source_body], nicks[0],
                          redact=ctx['lab']._redact)
        ctx['expected_messages'].extend(ctx['channel_messages'](source_body, nicks[0]))
        gui.wait_messages(p, db, ctx['expected_messages'])
        own_body = ('\x04D94577,D94577OWNLEFT\x0f ' + ctx['own_nick'] +
                    ' \x042AB971,2AB971OWNRIGHT\x0f')
        guests[0].send(f'PRIVMSG {channel} :{own_body}')
        gui.receive_until(ctx['observer'], 'PRIVMSG', [channel, own_body], nicks[0],
                          redact=ctx['lab']._redact)
        ctx['expected_messages'].extend(ctx['channel_messages'](own_body, nicks[0]))
        gui.wait_messages(p, db, ctx['expected_messages'])
        own_a, own_b = _policy_wait_box(ctx, (217, 69, 119)), _policy_wait_box(ctx, (42, 185, 113))
        assert abs(own_a[1] - own_b[1]) <= 1, ('Own nick reference wrapped', own_a, own_b)
        author_colors = {
            nicks[0]: _native_word_ink(ctx, author_box(nicks[0], _policy_wait_box(ctx, (45, 155, 217)))),
            ctx['own_nick']: _native_word_ink(ctx, (own_a[2] + 2, own_a[1], own_b[0] - 2, own_a[3])),
        }

        def typed_ink(raw, label):
            gui.clear_draft()
            gui._composer_native(w, '', 0, 0)
            start = gui.native_input_rectangle(w)
            _refresh_clipboard(ctx, raw.encode('utf-8'), 'text/plain;charset=utf-8')
            gui.framed_keys(w, 'ctrl+v')
            size = len(raw.encode('utf-8'))
            state = gui._composer_native(w, raw, size, size)
            end = state['caret']
            assert start[1] == end[1] and end[0] > start[0], ('Unexpected composer wrap', raw, start, end)
            box = (start[0], start[1], end[0] - 2, start[1] + start[3])
            observed = _native_word_ink(ctx, box, padding=0)
            captures.append(str(_policy_capture(ctx, label)))
            return observed, state

        for nick in (nicks[0], ctx['own_nick']):
            observed, state = typed_ink(nick, 'composer-colored-' + nick)
            assert observed['ink'] == author_colors[nick]['ink'], (
                'Typed known nick does not use actual source IRC nick color', nick, observed, author_colors[nick])
            evidence.append({'phase': 'known-nick', 'nick': nick, 'native': state,
                             'composer': observed, 'author': author_colors[nick]})
        unknown, _ = typed_ink('UnknownNativeAmber', 'composer-unknown-nick')
        url, _ = typed_ink('https://localhost/NativeAmber', 'composer-url-exclusion')
        assert unknown['ink'] == url['ink'] != author_colors[nicks[0]]['ink'], (
            'Unknown token or URL substring incorrectly uses known nick color', unknown, url, author_colors)
        known_ink = author_colors[nicks[0]]['ink']
        assert known_ink not in unknown['core_colors'] and known_ink not in url['core_colors'], (
            'A known nickname substring was colored inside an unknown token or URL', unknown, url)
        explicit_reference, _ = typed_ink('\x0304Ordinary\x0f', 'composer-explicit-color-reference')
        explicit_nick, _ = typed_ink('\x0304NativeAmber\x0f', 'composer-explicit-color-nick')
        assert explicit_reference['ink'] == explicit_nick['ink'], (
            'Known-nick projection replaced explicit mIRC foreground', explicit_reference, explicit_nick)
        raw = 'NativeAmber \x02bold café\x02 e\u0301 猫🙂 \x0304NativeAmber\x0f'
        gui.clear_draft()
        _refresh_clipboard(ctx, raw.encode(), 'text/plain;charset=utf-8')
        gui.framed_keys(w, 'ctrl+v')
        raw_size = len(raw.encode())
        raw_state = gui._composer_native(w, raw, raw_size, raw_size)
        gui.command('undo', window=w)
        gui._composer_native(w, '', 0, 0)
        gui.command('redo', window=w)
        gui._composer_native(w, raw, raw_size, raw_size)
        gui.framed_keys(w, 'Return')
        ctx['receive']('PRIVMSG', [channel, raw], ctx['own_nick'])
        ctx['expected_messages'].extend(ctx['channel_messages'](raw, ctx['own_nick']))
        gui.wait_messages(p, db, ctx['expected_messages'])
        gui.framed_keys(w, 'alt+p')
        recalled = gui._composer_native(w, raw, raw_size, raw_size)
        evidence.append({'phase': 'raw-format-undo-wire-history', 'raw': raw,
                         'utf8_bytes': raw_size, 'native_before_send': raw_state, 'history': recalled})
        gui.clear_draft()

        with sqlite3.connect(f'file:{db}?mode=ro', uri=True) as connection:
            own_before_completion = connection.execute(
                'SELECT buffer,id,text FROM messages WHERE own=1 ORDER BY buffer,seq').fetchall()
        prefix = 'NativeA'
        gui.type_text(prefix)
        before = gui._composer_native(w, prefix, len(prefix), len(prefix))
        x, y, width, _ = gui.read_layout(prefs)['conversation']['rect']
        region = (round(x), round(y), round(x + width), round(before['caret'][1] - 8))
        baseline = _policy_crop(gui.native_rgb_capture(w), region)
        gui.framed_keys(w, 'Tab')
        gui._composer_native(w, prefix, len(prefix), len(prefix))
        painted = _policy_crop(gui.native_rgb_capture(w), region)
        dimensions = (region[2] - region[0], region[3] - region[1])
        difference = ImageChops.difference(Image.frombytes('RGB', dimensions, baseline),
                                           Image.frombytes('RGB', dimensions, painted)).getbbox()
        assert difference, 'Two-candidate completion did not paint an actual popup'
        popup_region = (region[0] + difference[0], region[1] + difference[1],
                        difference[2] - difference[0], difference[3] - difference[1])
        def popup_labels(label):
            from difflib import SequenceMatcher
            from itertools import permutations
            words = _policy_words(ctx, label, popup_region)
            def similarity(text, nick):
                return SequenceMatcher(None, text.strip().casefold(), nick.casefold()).ratio()
            rows = sorted([(text, box) for text, box in words
                           if max(similarity(text, nick) for nick in nicks) >= .8],
                          key=lambda item: item[1][1])
            assert len(rows) == 2 and rows[0][1][3] < rows[1][1][1], (
                'No two genuine popup-bound completion label rows', words, popup_region)
            assignments = []
            for assignment in permutations(nicks):
                scores = [similarity(text, nick) for (text, _), nick in zip(rows, assignment)]
                if min(scores) >= .8:
                    assignments.append((sum(scores), assignment))
            assignments.sort(reverse=True)
            assert assignments and (len(assignments) == 1 or assignments[0][0] > assignments[1][0]), (
                'Ambiguous owned-nick bijection for actual popup OCR', rows, assignments)
            return [(nick, box) for (_, box), nick in zip(rows, assignments[0][1])]
        matched_rows = popup_labels('composer-popup-two-candidates')
        labels = [box for _, box in matched_rows]
        normal = _native_word_ink(ctx, labels[1])
        gui.move_pointer(w, (labels[1][0] + labels[1][2]) / 2, (labels[1][1] + labels[1][3]) / 2)
        # A native screencopy samples the hover frame without an OCR locator
        # parking the pointer elsewhere and accidentally clearing hover.
        def hover_painted():
            value = _native_word_ink(ctx, labels[1])
            return value if value['plate'] != normal['plate'] else False
        hovered = gui.SESSION.until('actual hovered completion plate', hover_painted)
        captures.append(str(_policy_capture(ctx, 'composer-popup-hover-contrast')))
        gui.move_pointer(w, 8, 8)
        gui.framed_keys(w, 'Down')
        selected = _native_word_ink(ctx, labels[1])
        for phase, measured in (('normal', normal), ('hover', hovered), ('selected', selected)):
            assert measured['contrast'] >= 4.5, (
                'Actual native completion foreground/plate contrast below 4.5', phase, measured)
            evidence.append({'phase': 'popup-' + phase, **measured})
        gui._composer_native(w, prefix, len(prefix), len(prefix))
        gui.framed_keys(w, 'Return')
        enter_raw = matched_rows[1][0] + ': '
        enter_state = gui._composer_native(w, enter_raw, len(enter_raw.encode()), len(enter_raw.encode()))
        gui.command('undo', window=w)
        gui._composer_native(w, prefix, len(prefix), len(prefix))
        gui.framed_keys(w, 'Tab')
        reopened_rows = popup_labels('composer-popup-reopened-for-tab')
        gui.framed_keys(w, 'Tab')
        tab_raw = reopened_rows[0][0] + ': '
        tab_state = gui._composer_native(w, tab_raw, len(tab_raw.encode()), len(tab_raw.encode()))
        gui.save_state(p, prefs, db)
        with sqlite3.connect(f'file:{db}?mode=ro', uri=True) as connection:
            own_after_completion = connection.execute(
                'SELECT buffer,id,text FROM messages WHERE own=1 ORDER BY buffer,seq').fetchall()
        assert own_after_completion == own_before_completion, 'Completion acceptance sent chat instead of raw edit'
        evidence.append({'phase': 'popup-exact-owned-nick-acceptance',
                         'matched_rows': matched_rows, 'enter': enter_state,
                         'reopened_rows': reopened_rows, 'tab': tab_state,
                         'no_own_history_added': True})
        gui.clear_draft()
        ime_text = gui.probe_real_ime(w, Path(ctx['destination']), ctx['cleanup'],
                                      gui.read_layout(prefs), ctx['args'].zoom, method=ctx['input_method'])
        gui.framed_keys(w, 'Return')
        ctx['receive']('PRIVMSG', [channel, ime_text], ctx['own_nick'])
        ctx['expected_messages'].extend(ctx['channel_messages'](ime_text, ctx['own_nick']))
        gui.wait_messages(p, db, ctx['expected_messages'])
        return _refresh_return(ctx, 'composer-affordances', {
            'evidence': evidence, 'captures': captures, 'palette': str(ctx['args'].palette),
            'popup_rows': labels, 'popup_native_difference': difference,
            'unknown': unknown, 'url': url, 'explicit_reference': explicit_reference,
            'explicit_nick': explicit_nick, 'ime_exact_wire': ime_text})


def _probe_logo_alignment(ctx):
    p, w = ctx['proc'], ctx['window']
    rails = []
    for mode in ('hidden', 'compact', 'expanded'):
        gui.set_navigation(p, w, ctx['preferences'], ctx['database_path'], mode, 'logo-' + mode)
        rails.append({'mode': mode, 'rail': gui.read_layout(ctx['preferences'])['rail']['rect'],
                      'capture': str(_policy_capture(ctx, 'logo-' + mode))})
        with sqlite3.connect(f'file:{ctx["database_path"]}?mode=ro', uri=True) as connection:
            rails[-1]['core_unread_channels'] = connection.execute(
                "SELECT id,unread,read_marker FROM buffers WHERE kind='channel' AND unread>0 ORDER BY id").fetchall()
        if mode == 'hidden':
            rails[-1]['temporary_compact_avy'] = str(gui.open_avy(
                w, ctx['preferences'], Path(ctx['destination']), 'hidden-unread-status'))
            gui.framed_keys(w, 'Escape')
            rails[-1]['restored_hidden'] = str(_policy_capture(ctx, 'hidden-unread-status-restored'))
    return {'captures': rails,
            'geometry_contract': 'Native UI integration owns exact Image/Rect bounds and texture identity; '
                                 'transparent asset silhouette is not falsely equated with avatar plate.'}


def run_logo_alignment(ctx):
    return _refresh_return(ctx, 'logo-alignment', _probe_logo_alignment(ctx))


def run_overlay_refinements(ctx):
    """Real unreserved reply overlap, fixed native Input, transparent history and /me."""
    p, w, prefs, db = ctx['proc'], ctx['window'], ctx['preferences'], ctx['database_path']
    identity = ctx['query_id']
    _policy_focus(ctx, identity)
    _policy_effects(ctx, fx='off')
    _policy_chat(ctx, fools=[], receive=True)
    _policy_typing_cap(ctx)
    gui.set_message_style(p, w, prefs, db, ctx['args'], ctx['destination'],
                          identity, 'irc', label='overlay-history-style')
    # Actual retained wire rows keep the followed newest row at the viewport bottom.
    for index in range(40):
        _policy_send(ctx, '\x04B34FD5,B34FD5' + 'H' * 60 + '\x0f ' + str(index), durable=False)
    gui.wait_messages(p, db, ctx['expected_messages'])
    source_body = '\x0419A7D3,19A7D3REPLYSOURCE\x0f ' + ('OverlayExcerpt café e\u0301 中 ' * 9)
    _policy_send(ctx, source_body)
    _policy_send(ctx, '\x042AB971,2AB971FONTANCHOR\x0f typesetter')
    upload.focus_composer(w)
    draft = 'typesetter'
    gui._composer_native(w, '', 0, 0)
    gui.type_text(draft)
    gui.framed_keys(w, 'Left', 'Right')
    size = len(draft.encode())
    before = gui._composer_native(w, draft, size, size)
    # The newest wide swatch crosses the card's text-free area without any scroll.
    _policy_send(ctx, '\x04D37925,D37925' + 'H' * 60 + '\x0f newest overlay witness')
    newest = _policy_wait_box(ctx, (211, 121, 37))
    anchor = _policy_wait_box(ctx, (42, 185, 113))
    source_marker = _policy_wait_box(ctx, (25, 167, 211))
    markers = {'newest': ((211, 121, 37), newest),
               'fontanchor': ((42, 185, 113), anchor),
               'source': ((25, 167, 211), source_marker)}
    captures = []

    def overlap_observation(label, *, obscured=False):
        native = gui._composer_native(w, draft, size, size)
        rectangle = gui.read_layout(prefs)['conversation']['rect']
        frame = gui.native_rgb_capture(w)
        path = Path(ctx['destination']).with_name('overlay-overlap-' + label + '.png')
        Image.frombytes('RGB', (frame['width'], frame['height']), frame['pixels']).save(path)
        captures.append(str(path))
        observed = {}
        for name, (rgb, box) in markers.items():
            roi = (box[0] - 1, box[1] - 1, box[2] - box[0] + 2, box[3] - box[1] + 2)
            parts = gui.native_marker_components(frame, roi, rgb)
            assert parts, ('Actual marker vanished; absence is not overlap evidence', label, name, box, str(path))
            bounds = (min(item['bbox'][0] for item in parts), min(item['bbox'][1] for item in parts),
                      max(item['bbox'][2] for item in parts), max(item['bbox'][3] for item in parts))
            assert (box[0] <= bounds[0] < bounds[2] <= box[2]
                    and box[1] <= bounds[1] < bounds[3] <= box[3]), (
                'Actual marker moved outside its saved native ROI', label, name, box, parts, str(path))
            if name == 'newest' or not obscured:
                assert bounds == tuple(box), ('Actual marker geometry changed', label, name, box, parts, str(path))
            if not obscured:
                assert len(parts) == 1 and parts[0]['full'], (label, name, parts, str(path))
            observed[name] = {'original_bbox': box, 'surviving_components': parts, 'saved_roi': roi}
        return {'native_composer': native, 'conversation': rectangle,
                'markers': observed, 'capture': str(path)}

    closed = overlap_observation('before')
    layout_before = closed['conversation']
    reply = gui.begin_native_reply(w, gui.read_layout(prefs), ctx['args'].zoom,
                                    Path(ctx['destination']), label='overlay-two-line-reply')
    gui._composer_native(w, draft, size, size)
    # The context-menu pointer can remain over the rich source swatch. Move
    # it into the actual native Input without clicking before measuring ink.
    pointer_offset = gui.GUI_DEBUG[w][1].stat().st_size
    gui.move_pointer(w, before['caret'][0], before['caret'][1] + before['caret'][3] / 2)
    gui.SESSION.until('native pointer leaves the measured source swatch', lambda:
        re.search(r'wl_pointer[@#]\d+\.(?:motion|enter)\(',
                  gui.GUI_DEBUG[w][1].read_bytes()[pointer_offset:].decode(errors='replace')))
    opened = overlap_observation('open', obscured=True)
    assert opened['native_composer'] == closed['native_composer'], 'Reply changed native composer geometry/draft'
    assert opened['conversation'] == layout_before, 'Reply changed conversation geometry'
    excerpt_region = _policy_reply_region(ctx, 'overlay-two-lines')
    preview_parts = [item['bbox'] for item in gui.native_marker_components(
        gui.native_rgb_capture(w), layout_before, (25, 167, 211))
        if item['full'] and item['bbox'][1] >= excerpt_region[1] and item['bbox'][3] < before['caret'][1]]
    assert len(preview_parts) == 1, ('No unique actual rich preview row marker', preview_parts)
    preview_marker = preview_parts[0]
    raw_reply_words = _policy_words(ctx, 'overlay-two-lines-raw', excerpt_region)
    body_words = [(text, box) for text, box in raw_reply_words
                  if box[0] >= preview_marker[0]
                  and preview_marker[1] < (box[1] + box[3]) / 2 < preview_marker[3]]
    assert body_words, ('No actual preview text bounds beside rich marker', raw_reply_words, preview_marker)
    body_right = max(box[2] for _, box in body_words)
    excerpt_region = (preview_marker[0], excerpt_region[1],
                      body_right - preview_marker[0], excerpt_region[3])
    # Preserve the full vertical inspection band and all small/Unicode words;
    # only actual inner text bounds exclude the rounded border and Cancel.
    reply_words = _policy_words(ctx, 'overlay-two-lines', excerpt_region)
    excerpts = [box for text, box in reply_words if 'overlayexcerpt' in text.casefold()]
    assert excerpts, ('No actual source excerpt in native reply card', reply_words)
    baselines = []
    for _, box in sorted(reply_words, key=lambda item: item[1][1]):
        center = (box[1] + box[3]) / 2
        if not any(abs(center - previous) <= max(3, (box[3] - box[1]) / 2) for previous in baselines):
            baselines.append(center)
    assert len(baselines) == 1, ('Reply excerpt spans more than one native glyph row', reply_words, baselines)
    heading_words = _policy_words(ctx, 'overlay-heading-reference')
    heading = max((box for text, box in heading_words if text.casefold() == 'replying'
                   and box[3] < excerpt_region[1]), key=lambda box: box[3])
    typing_top = round(max(box[3] for _, box in reply_words)) + 1
    typing_box = (round(layout_before[0]), typing_top,
                  round(layout_before[0] + layout_before[2]), round(before['caret'][1]))
    assert typing_box[1] < typing_box[3], ('No measured typing band between reply and native Input', typing_box)
    gui.move_pointer(w, layout_before[0] + layout_before[2] / 2, layout_before[1] + 1)

    def typing_pixels():
        return _policy_crop(gui.native_rgb_capture(w), typing_box)

    def save_typing_pixels(label, pixels):
        path = Path(ctx['destination']).with_name('overlay-typing-' + label + '.png')
        Image.frombytes('RGB', (typing_box[2] - typing_box[0], typing_box[3] - typing_box[1]), pixels).save(path)
        captures.append(str(path))
        return str(path)

    typing_baseline = typing_pixels()
    typing_before_capture = save_typing_pixels('before', typing_baseline)
    def typing_visible():
        _policy_typing(ctx, 'active')  # Keep genuine activity alive during native capture.
        pixels = typing_pixels()
        changed = [index // 3 for index in range(0, len(pixels), 3)
                   if pixels[index:index + 3] != typing_baseline[index:index + 3]]
        if not changed:
            return False
        width = typing_box[2] - typing_box[0]
        bounds = (typing_box[0] + min(index % width for index in changed),
                  typing_box[1] + min(index // width for index in changed),
                  typing_box[0] + max(index % width for index in changed) + 1,
                  typing_box[1] + max(index // width for index in changed) + 1)
        assert typing_box[1] < bounds[1] < bounds[3] < typing_box[3], (
            'Typing transition crosses the measured reply/Input boundaries', typing_box, bounds)
        return {'measured_band': typing_box, 'changed_bbox': bounds, 'changed_pixels': len(changed),
                'before_capture': typing_before_capture, 'active_capture': save_typing_pixels('active', pixels)}
    typing = gui.SESSION.until('real TAGMSG paints the measured band below reply and above Input', typing_visible)
    assert gui.native_input_rectangle(w) == before['caret']
    overlap_observation('typing', obscured=True)
    assert gui._composer_native(w, draft, size, size)['caret'] == before['caret']
    _policy_typing(ctx, 'done')
    def typing_restored():
        pixels = typing_pixels()
        return pixels if pixels == typing_baseline else False
    typing_done = gui.SESSION.until('real done exactly restores the measured typing band', typing_restored)
    typing['done_capture'] = save_typing_pixels('done', typing_done)
    typing['done_restores_exact_baseline'] = True
    captures.append(str(_policy_capture(ctx, 'overlay-fixed-two-lines')))

    # No wheel or navigation occurs between the followed newest row and this
    # opening observation. Actual surviving RGB, not an absent/clipped marker,
    # must occupy the real heading/excerpt band immediately.
    card_band = (heading[1], max(box[3] for _, box in reply_words))
    surviving = opened['markers']['newest']['surviving_components']
    behind = [item for item in surviving
              if max(item['bbox'][1], card_band[0]) < min(item['bbox'][3], card_band[1])]
    assert behind, ('Followed newest actual marker does not occupy the open card band',
                    newest, card_band, surviving, opened['capture'])
    painted_behind = {'card_band': card_band, 'newest_marker': newest,
                      'surviving_components_in_band': behind, 'wheel_required': False,
                      'capture': opened['capture']}
    cancelled = gui.cancel_native_reply(w, ctx['destination'], label='overlay-cancel', expected_text=draft)
    captures.extend(cancelled['captures'])
    closed_again = overlap_observation('cancel')
    assert closed_again['native_composer'] == closed['native_composer'], \
        'Real Cancel reply changed native composer geometry or draft'
    assert closed_again['conversation'] == layout_before, 'Real Cancel reply changed conversation geometry'
    gui.clear_draft()
    upload.focus_composer(w)

    # Compare native regular/italic glyph masks and paint outside the action
    # glyph band in both display modes; no copied font name, RGB or card size.
    action_text = 'slanted'
    # First action in a genuinely fresh query guarantees a clock even in the
    # traditional minute-deduplicating lane, without fabricated server-time.
    actor_nick = 'Emoter'
    action_identity = 'Local/' + actor_nick.casefold()
    emoter = ctx['cleanup'].enter_context(gui.WireObserver(
        dict(ctx['lab'].endpoints['direct'], nick=actor_nick, username=actor_nick),
        ctx['lab'].ca_file, authenticate=False))
    emoter.send('CAP REQ :message-tags echo-message')
    emoter.wait(lambda command, params, prefix: command == 'CAP' and ('ACK' in params or 'NAK' in params))
    assert {'message-tags', 'echo-message'} <= emoter.capabilities, emoter.capabilities
    for wire_body in ('\x01ACTION ' + action_text + '\x01', action_text):
        emoter.send(f'PRIVMSG {ctx["own_nick"]} :{wire_body}')
        gui.receive_until(emoter, 'PRIVMSG', [ctx['own_nick'], wire_body], actor_nick,
                          redact=ctx['lab']._redact)
        ctx['expected_messages'].append((action_identity, actor_nick, action_text, 0, 0))
    gui.wait_messages(p, db, ctx['expected_messages'])
    gui.wait_rows(p, db, 'SELECT action FROM messages WHERE buffer=? AND text=? ORDER BY seq',
                  (action_identity, action_text), [(1,), (0,)])
    gui.select_native_buffer(p, w, prefs, db, ctx['args'], action_identity)
    gui._composer_native(w, '', 0, 0)
    actions = []
    for style in ('bubbles', 'irc'):
        gui.set_message_style(p, w, prefs, db, ctx['args'], ctx['destination'],
                              action_identity, style, label='overlay-action-' + style)
        upload.focus_composer(w)
        words = _policy_words(ctx, 'overlay-action-' + style)
        bodies = sorted([box for text, box in words if text.casefold() == action_text.casefold()], key=lambda box: box[1])
        assert len(bodies) == 2, ('No actual regular/action native glyph pair', style, words)
        action, regular = bodies
        actor = [box for text, box in words if text.rstrip(':').casefold() == actor_nick.casefold()
                 and box[2] < action[0] and box[1] <= (action[1] + action[3]) / 2 <= box[3]]
        assert actor, ('Action actor is not inline with body', style, words)
        actor = actor[-1]
        def glyph_mask(box):
            measured = _native_word_ink(ctx, box, padding=0)
            frame = gui.native_rgb_capture(w)
            data = _policy_crop(frame, box)
            vector = tuple(a - b for a, b in zip(measured['ink'], measured['plate']))
            norm = sum(value * value for value in vector)
            mask = bytes(255 if sum((a - b) * v for a, b, v in
                         zip(data[index:index + 3], measured['plate'], vector)) >= norm / 2 else 0
                         for index in range(0, len(data), 3))
            return ((box[2] - box[0], box[3] - box[1]), mask)
        assert glyph_mask(regular) != glyph_mask(action), (
            'Action body has the same native glyph shape as regular, not italic', style)
        x, _, width, _ = gui.read_layout(prefs)['conversation']['rect']
        frame = gui.native_rgb_capture(w)
        left, right = round(actor[0]), round(action[2])
        for row in (round(min(actor[1], action[1]) - 3), round(max(actor[3], action[3]) + 3)):
            reference = _policy_crop(frame, (round(x + width - 30), row, round(x + width - 29), row + 1))
            blank_columns = [column for column in range(left, right)
                             if not any(box[0] - 1 <= column <= box[2] + 1
                                        and box[1] - 1 <= row <= box[3] + 1 for _, box in words)]
            assert len(blank_columns) >= 20, ('No actual blank action padding to inspect', style, row)
            assert all(_policy_crop(frame, (column, row, column + 1, row + 1)) == reference
                       for column in blank_columns), ('Action still paints a bubble/card', style, row)
        glyph_height = max(actor[3] - actor[1], action[3] - action[1])
        if style == 'bubbles':
            time_left = min(actor[0], action[0]) - glyph_height
            time_top = max(actor[3], action[3]) + 1
            time_right = max(actor[2], action[2]) + glyph_height
            time_bottom = regular[1]
        else:
            time_left, time_right = x, max(actor[2], action[2]) + glyph_height
            time_top, time_bottom = action[1] - glyph_height, action[3] + glyph_height
        assert time_left < time_right and time_top < time_bottom, (style, actor, action, regular)
        time_rows = upload.visible_words(w, Path(ctx['destination']), 'overlay-action-time-' + style,
            region=(time_left, time_top, time_right - time_left, time_bottom - time_top), psm=11)
        timestamps = [(int(row['left']), int(row['top']),
                       int(row['left']) + int(row['width']), int(row['top']) + int(row['height']))
                      for row in time_rows if re.search(r'\d:\d', row['text'])]
        assert timestamps, ('First action timestamp missing in its measured native band', style, time_rows)
        stamp = min(timestamps, key=lambda box: abs((box[1] + box[3] - action[1] - action[3]) / 2))
        stamp_ink, body_ink = _native_word_ink(ctx, stamp), _native_word_ink(ctx, action)
        assert stamp_ink['contrast'] < body_ink['contrast'], (
            'Action timestamp is not visually muted against its actual native plate', stamp_ink, body_ink)
        actions.append({'style': style, 'regular': regular, 'italic_action': action,
                        'inline_actor': actor, 'timestamp': stamp,
                        'timestamp_ink': stamp_ink, 'body_ink': body_ink})
        captures.append(str(_policy_capture(ctx, 'overlay-action-no-bubble-' + style)))
    logo = _probe_logo_alignment(ctx)
    return _refresh_return(ctx, 'overlay-refinements', {
        'reply': reply,
        'two_line_excerpt': reply_words, 'typing_below_reply': typing,
        'input_caret_unchanged': before['caret'], 'history_anchor_unchanged': anchor,
        'transparent_history': painted_behind, 'actual_wheel_frames': [],
        'overlap_before_open_cancel': [closed, opened, closed_again], 'real_cancel': cancelled,
        'actions': actions, 'rail_captures': logo['captures'], 'captures': captures,
        'logo_geometry': logo['geometry_contract']})


def run_input_routing(ctx):
    """Focused native C-e EOL plus unchanged transcript-focus reaction wire proof."""
    _policy_focus(ctx, ctx['query_id'])
    _policy_effects(ctx, fx='off')
    _policy_chat(ctx, fools=[])
    _policy_typing_cap(ctx)
    rows = gui.probe_reaction_picker(
        ctx['proc'], ctx['window'], ctx['preferences'], ctx['database_path'], ctx['args'],
        Path(ctx['destination']), ctx['observer'], query_id=ctx['query_id'],
        query_target=ctx['query_target'], own_nick=ctx['own_nick'], lab=ctx['lab'], receive=ctx['receive'])
    ctx['expected_messages'].extend(rows)
    gui.wait_messages(ctx['proc'], ctx['database_path'], ctx['expected_messages'])
    return _refresh_return(ctx, 'input-routing', {
        'actual_source_rows': rows,
        'native_evidence': str(Path(ctx['destination']).with_name(
            Path(ctx['destination']).stem + '-reaction-picker-evidence.json')),
        'coverage': 'Multiline UTF-8 composer C-e stays on current line with exact raw bytes/no reaction '
                    'wire; actual transcript C-p then C-e still opens picker and sends validated reaction.'})


def run_activity_pins(ctx):
    """Actual chat recency, stable persisted full-ID pins and reorder-safe Avy."""
    p, w, prefs, db = ctx['proc'], ctx['window'], ctx['preferences'], ctx['database_path']
    channel, channel_id = ctx['channel'], ctx['channel_id']
    names = ('Amber', 'Cedar', 'Maple')
    # These ASCII fixture targets use IRC's casefolded query ID while retaining
    # their real mixed-case sender/visible label everywhere else.
    ids = {name: 'Local/' + name.casefold() for name in names}
    ids[channel] = channel_id
    evidence, clients = [], {}
    _policy_focus(ctx, channel_id)
    _policy_effects(ctx, fx='off')
    _policy_chat(ctx, fools=[], receive=True)
    gui.set_navigation(p, w, prefs, db, 'expanded', 'activity-pins-expanded')
    original_pins = list(tomllib.loads(Path(prefs).read_text()).get('presentation', {}).get('pinned_buffers', []))
    for name in names:
        peer = ctx['cleanup'].enter_context(gui.WireObserver(
            dict(ctx['lab'].endpoints['direct'], nick=name, username=name),
            ctx['lab'].ca_file, authenticate=False))
        peer.send('CAP REQ :message-tags echo-message')
        peer.wait(lambda command, params, prefix: command == 'CAP' and ('ACK' in params or 'NAK' in params))
        assert {'message-tags', 'echo-message'} <= peer.capabilities, peer.capabilities
        clients[name] = peer

    def incoming(name, text, *, other=False, action=False):
        recipient = ctx['other_nick'] if other else ctx['own_nick']
        wire = '\x01ACTION ' + text + '\x01' if action else text
        clients[name].send(f'PRIVMSG {recipient} :{wire}')
        gui.receive_until(clients[name], 'PRIVMSG', [recipient, wire], name, redact=ctx['lab']._redact)
        identity = ('Other/' if other else 'Local/') + name.casefold()
        ctx['expected_messages'].append((identity, name, text, 0, 0))
        gui.wait_messages(p, db, ctx['expected_messages'])

    def positions(label):
        from difflib import SequenceMatcher
        layout = gui.read_layout(prefs)
        rectangle = layout['channels']['rect']
        rows = upload.visible_words(w, Path(ctx['destination']), label, region=rectangle, psm=6)
        words = [(row['text'].strip(), (int(row['left']), int(row['top']),
                  int(row['left']) + int(row['width']), int(row['top']) + int(row['height']))) for row in rows]
        owned = {name: {name.casefold(), name.lstrip('#').casefold()} for name in (*names, channel)}
        def missing_labels():
            return [name for name, targets in owned.items()
                    if not any(text.casefold() in targets for text, _ in words)]
        def add_words(rows):
            for row in rows:
                text = row['text'].strip()
                box = (int(row['left']), int(row['top']),
                       int(row['left']) + int(row['width']), int(row['top']) + int(row['height']))
                if not any(text.casefold() == old.casefold() and max(box[1], previous[1]) < min(box[3], previous[3])
                           and max(box[0], previous[0]) < min(box[2], previous[2]) for old, previous in words):
                    words.append((text, box))
        if missing_labels():
            add_words(upload.visible_words(w, Path(ctx['destination']), label + '-sparse',
                                           region=rectangle, psm=11))
        # Block OCR can omit the selected blue row entirely. Locate actual
        # wide, filled native row plates below the observed server header,
        # then recognize their label column as one line (no assumed row order).
        if missing_labels() and layout['active_buffer'] in ids.values():
            server = tomllib.loads(Path(prefs).read_text())['workspace']['selected_server']
            headers = [box for text, box in words if text.casefold() == server.casefold()]
            labels = [box for text, box in words if any(text.casefold() in targets for targets in owned.values())]
            if headers and labels:
                header_bottom = max(box[3] for box in headers)
                label_left = min(box[0] for box in labels)
                glyph_height = max(box[3] - box[1] for box in labels)
                label_right = max(box[2] for box in labels)
                trailing = [box[0] for _, box in words if box[0] > label_right and
                            any(max(box[1], known[1]) < min(box[3], known[3]) for known in labels)]
                frame = gui.native_rgb_capture(w)
                native = Image.frombytes('RGB', (frame['width'], frame['height']), frame['pixels'])
                x, y, width, height = rectangle
                plate = native.crop(tuple(map(round, (x, y, x + width, y + height))))
                colors = plate.getcolors(plate.width * plate.height)
                background = max(colors)[1]
                for count, rgb in colors:
                    if rgb == background or count < width * glyph_height:
                        continue
                    for item in gui.native_marker_components(frame, rectangle, rgb):
                        left, top, right, bottom = item['bbox']
                        area = (right - left) * (bottom - top)
                        if (top < header_bottom or right - left < width / 2
                                or not glyph_height <= bottom - top <= (right - left) / 2
                                or item['pixels'] < area / 2 or right <= label_left):
                            continue
                        # Keep real whitespace around the text, but exclude
                        # rounded card edges and the observed unread column.
                        inset = (bottom - top) / 2
                        text_left = max(left + inset, label_left - glyph_height / 2)
                        text_right = min(right - inset, min(trailing) if trailing else right)
                        if text_right <= text_left:
                            continue
                        add_words(upload.visible_words(w, Path(ctx['destination']), label + '-selected-row',
                            region=(text_left, top, text_right - text_left, bottom - top), psm=7, min_confidence=0))
        found = {}
        for name in (*names, channel):
            accepted = {name.casefold(), name.lstrip('#').casefold()}
            matches = sorted((max(SequenceMatcher(None, text.casefold(), target).ratio()
                                  for target in accepted), text, box) for text, box in words)
            if not matches or matches[-1][0] < .75 or (len(matches) > 1 and matches[-1][0] == matches[-2][0]):
                print('Native activity label OCR diagnostic:', label, name, words, matches, flush=True)
                return False
            found[name] = matches[-1][2]
        ordered = sorted(found.values(), key=lambda box: box[1])
        if any(a[3] >= b[1] for a, b in zip(ordered, ordered[1:])):
            print('Native activity overlapping-label OCR diagnostic:', label, words, found, flush=True)
            return False
        return found, words

    def order(expected, label):
        gui.save_state(p, prefs, db)
        def observed():
            snapshot = positions(label)
            if not snapshot:
                return False
            found, words = snapshot
            actual = sorted(found, key=lambda name: found[name][1])
            if actual != expected:
                print('Native activity order diagnostic:', label, actual, expected, words, found, flush=True)
                return False
            return found
        found = gui.SESSION.until('actual visible activity/pin order ' + label, observed)
        evidence.append({'phase': label, 'order': expected, 'native_labels': found})
        return found

    def pins(expected):
        gui.save_state(p, prefs, db)
        actual = tomllib.loads(Path(prefs).read_text()).get('presentation', {}).get('pinned_buffers', [])
        assert actual == expected + original_pins, ('Persisted pin source IDs/order differ', actual, expected)

    def pin(name, enabled=True):
        _policy_focus(ctx, ids[name])
        gui.command('pin-buffer' if enabled else 'unpin-buffer', window=w)
        gui._composer_native(w, '', 0, 0)

    for name in names:
        incoming(name, name + ' first real activity')
    order(['Maple', 'Cedar', 'Amber', channel], 'three-real-queries')
    _policy_send(ctx, 'New actual channel activity', target=channel)
    order([channel, 'Maple', 'Cedar', 'Amber'], 'channel-interleaves-with-queries')
    incoming('Amber', 'Amber latest incoming activity')
    order(['Amber', channel, 'Maple', 'Cedar'], 'incoming-promotes-original-source')
    _policy_focus(ctx, ids['Cedar'])
    order(['Amber', channel, 'Maple', 'Cedar'], 'focus-alone-does-not-promote')
    own = 'Own Cedar activity café'
    gui.type_text(own)
    gui._composer_native(w, own, len(own.encode()), len(own.encode()))
    gui.framed_keys(w, 'Return')
    gui.receive_until(clients['Cedar'], 'PRIVMSG', ['Cedar', own], ctx['own_nick'], redact=ctx['lab']._redact)
    ctx['expected_messages'].append((ids['Cedar'], ctx['own_nick'], own, 1, 0))
    gui.wait_messages(p, db, ctx['expected_messages'])
    order(['Cedar', 'Amber', channel, 'Maple'], 'own-chat-promotes-original-source')
    incoming('Maple', 'Maple actual emote activity', action=True)
    order(['Maple', 'Cedar', 'Amber', channel], 'actual-action-promotes-source')

    _policy_focus(ctx, ids['Amber'])
    def typing(state):
        peer = clients['Amber']
        assert gui.client_tags_allowed(peer, '+typing'), peer.isupport
        peer.send(f'@+typing={state} TAGMSG {ctx["own_nick"]}')
        peer.wait(lambda command, params, prefix: command == 'TAGMSG' and
                  params == [ctx['own_nick']] and prefix.split('!', 1)[0] == 'Amber' and
                  peer.last_tags.get('+typing') == state)
    def typing_visible():
        x, y, width, height = gui.read_layout(prefs)['conversation']['rect']
        caret = gui.native_input_rectangle(w)
        return [box for text, box in _policy_words(ctx, 'activity-positive-typing')
                if text.casefold() == 'amber' and x <= box[0] < x + width
                and y + height / 2 < box[1] < box[3] < caret[1]]
    typing('active')
    typing_box = gui.SESSION.until('real Amber typing appears without chat arrival', typing_visible)
    order(['Maple', 'Cedar', 'Amber', channel], 'typing-does-not-reshuffle')
    typing('done')
    gui.SESSION.until('real done removes Amber typing', lambda: not typing_visible())
    clients['Amber'].send('JOIN ' + channel)
    gui.receive_until(ctx['observer'], 'JOIN', [channel], 'Amber', redact=ctx['lab']._redact)
    gui.wait_rows(p, db, 'SELECT activity FROM messages WHERE buffer=? AND nick=? AND presence=1 ORDER BY seq',
                  (channel_id, 'Amber'), [(1,)])
    order(['Maple', 'Cedar', 'Amber', channel], 'real-join-does-not-reshuffle')
    clients['Amber'].send('PART ' + channel + ' :private activity proof')
    gui.receive_until(ctx['observer'], 'PART', [channel, 'private activity proof'], 'Amber', redact=ctx['lab']._redact)
    gui.wait_rows(p, db, 'SELECT activity FROM messages WHERE buffer=? AND nick=? AND presence=1 ORDER BY seq',
                  (channel_id, 'Amber'), [(1,), (2,)])
    order(['Maple', 'Cedar', 'Amber', channel], 'real-part-does-not-reshuffle')

    pin('Amber')
    pins([ids['Amber']])
    order(['Amber', 'Maple', 'Cedar', channel], 'first-pin-above-newer-chat')
    pin('Maple')
    pins([ids['Maple'], ids['Amber']])
    order(['Maple', 'Amber', 'Cedar', channel], 'newest-pin-first')
    incoming('Cedar', 'Cedar newer than pinned sources')
    _policy_send(ctx, 'Channel newer than unpinned Cedar', target=channel)
    incoming('Amber', 'Pinned Amber receives newer activity without pin reorder')
    order(['Maple', 'Amber', channel, 'Cedar'], 'pins-stay-stable-through-new-arrivals')
    pin('Maple', False)
    pins([ids['Amber']])
    order(['Amber', channel, 'Cedar', 'Maple'], 'unpin-returns-to-actual-activity-position')
    pin('Maple')
    pins([ids['Maple'], ids['Amber']])
    order(['Maple', 'Amber', channel, 'Cedar'], 'repin-newest-first')

    # Same nick on another configured network is a distinct original source.
    incoming('Amber', 'Other network Amber source identity', other=True)
    other_amber = 'Other/' + 'Amber'.casefold()
    gui.select_native_buffer(p, w, prefs, db, ctx['args'], other_amber)
    gui._composer_native(w, '', 0, 0)
    popup = gui.open_title_menu(w, gui.title_actions(gui.read_layout(prefs), ctx['args'].zoom, channel=False))
    menu_words = _policy_words(ctx, 'activity-other-source-pin-menu')
    choices = [box for text, box in menu_words if text.casefold() == 'pin']
    assert len(choices) == 1 and not any(text.casefold() == 'unpin' for text, _ in menu_words), (
        'Local Amber pin leaked into Other Amber policy', menu_words)
    # Deliver a real list reorder while the native menu exists. Retiring the
    # menu is allowed; a still-live callback must keep its original source ID.
    _policy_send(ctx, 'Channel arrival while Other Amber pin menu is open', target=channel)
    trace = gui.GUI_DEBUG[w][1].read_text(errors='replace')[popup['creation_offset']:]
    retired = re.search(rf"xdg_popup[@#]{popup['popup']}\.destroy\(", trace) is not None
    if retired:
        gui.command('pin-buffer', window=w)
    else:
        box = choices[0]
        gui.click(w, (box[0] + box[2]) / 2, (box[1] + box[3]) / 2)
    expected_pins = [other_amber, ids['Maple'], ids['Amber']]
    pins(expected_pins)
    evidence.append({'phase': 'cross-network-original-pin-callback', 'source': other_amber,
                     'native_menu_retired_on_reorder': retired, 'pins': expected_pins})

    _policy_focus(ctx, ids['Maple'])
    order(['Maple', 'Amber', channel, 'Cedar'], 'other-network-pin-does-not-change-local-order')
    draft = 'activity draft café 猫🙂'
    gui.type_text(draft)
    byte_end = len(draft.encode())
    gui._composer_native(w, draft, byte_end, byte_end)
    # Existing Avy uses unique target initials; prove the selected full ID after
    # an actual unpinned row moves while the visible overlay owns keyboard focus.
    with sqlite3.connect(f'file:{db}?mode=ro', uri=True) as connection:
        targets = connection.execute(
            "SELECT id,target FROM buffers WHERE server='Local' AND kind IN ('channel','query')").fetchall()
    initials = [(identity, re.search('[a-z0-9]', target.casefold()).group()) for identity, target in targets]
    hint = next(initial for identity, initial in initials if identity == ids['Cedar'])
    assert sum(initial == hint for _, initial in initials) == 1, initials
    avy = gui.open_avy(w, prefs, Path(ctx['destination']), 'activity-reorder-original-identity')
    incoming('Cedar', 'Cedar changes unpinned row index while Avy is open')
    gui.framed_keys(w, hint)
    selected = gui.save_state(p, prefs, db)
    assert selected[1] == (ids['Cedar'],), ('Avy followed stale row index instead of original source', selected)
    order(['Maple', 'Amber', 'Cedar', channel], 'avy-selects-original-after-reorder')
    gui.select_native_buffer(p, w, prefs, db, ctx['args'], ids['Maple'])
    gui._composer_native(w, draft, byte_end, byte_end)

    # Restart the actual app against precisely the same owned profile/database;
    # never insert settings, activity timestamps or history through a fixture hook.
    gui.save_state(p, prefs, db)
    target = Path(ctx['destination']).with_name('activity-pins-before-restart.bmp')
    out, err = gui.finish_gui(p, w, ctx['screenshot'], target, ctx['args'].size)
    ctx['logs'].append(('activity-pins-before-restart', out, err))
    departed = {ctx['own_nick'], ctx['other_nick']}
    while departed:
        _, _, prefix = ctx['observer'].wait(lambda command, params, prefix:
            command == 'QUIT' and prefix.split('!', 1)[0] in departed, timeout=15)
        departed.remove(prefix.split('!', 1)[0])
    screenshot = Path(ctx['screenshot']).with_name('activity-pins-restart.bmp')
    p, w = gui.launch_gui(ctx['binary'], screenshot, ctx['env'], ctx['args'].size, ctx['logs'])
    ctx.update(proc=p, window=w, screenshot=screenshot)
    gui.wait_joins(ctx['observer'], channel, (ctx['own_nick'], ctx['other_nick']))
    gui.select_native_buffer(p, w, prefs, db, ctx['args'], ids['Maple'])
    gui._composer_native(w, draft, byte_end, byte_end)
    pins(expected_pins)
    order(['Maple', 'Amber', 'Cedar', channel], 'restart-retains-actual-activity-and-stable-pins')
    return _refresh_return(ctx, 'activity-pins', {
        'evidence': evidence, 'positive_typing': typing_box, 'pins': expected_pins,
        'avy_overlay': str(avy), 'avy_hint': hint, 'avy_original_source': ids['Cedar'],
        'draft_after_restart': draft,
        'capture': str(_policy_capture(ctx, 'activity-pins-restarted')),
        'history_replay': 'Real restart/reconnect and retained order verified; exhaustive historical replay '
                          'classification is Core regression-owned. No unsupported CHATHISTORY is forced.'})


def run_minimum_height(ctx):
    """One real Settings/Wayland scenario; no model hooks or font-size assumptions."""
    from contextlib import nullcontext

    p, w = ctx['proc'], ctx['window']
    prefs, db = Path(ctx['preferences']), ctx['database_path']
    debug = gui.GUI_DEBUG[w][1]
    top = gui.main_surface_ids(debug)[2]
    request = re.compile(rf'xdg_toplevel[@#]{top}\.set_min_size\((\d+),\s*(\d+)\)')
    output = gui.display_geometry()
    original = tomllib.loads(prefs.read_text())['appearance']
    assert original['scale'] == original['message_scale'] == 1, original

    def requests():
        return [tuple(map(int, item)) for item in request.findall(debug.read_text(errors='replace'))]

    initial = requests()
    assert initial and initial[0][0] == 720 and initial[0][1] >= 480, initial
    assert all(a != b for a, b in zip(initial, initial[1:])), initial
    # The private output, not a forced undersized client, must fit 2x plus chrome.
    gui.resize_panel_viewport(w, (max(output[0], 2000), max(output[1], 1600)))
    _policy_focus(ctx, ctx['query_id'])
    gui.set_message_style(p, w, prefs, db, ctx['args'], ctx['destination'],
                          ctx['query_id'], 'irc', label='scale-known-dense-style')
    _policy_effects(ctx, fx='off')
    _policy_chat(ctx, fools=[], receive=False)
    word = 'ScaleGlyph'
    color = (42, 185, 113)
    _policy_send(ctx, '\x042AB971,2AB971LEFTLEFT\x0f ' + word +
                 ' \x04B34FD5,B34FD5RGHTRGHT\x0f')
    upload.focus_composer(w)
    gui.clear_draft()
    gui.type_text(word)
    end = len(word.encode('utf-8'))
    gui._composer_native(w, word, end, end)
    gui.wait_read_marker(p, db, ctx['query_id'])
    gui.save_state(p, prefs, db)

    def history():
        with sqlite3.connect(f'file:{db}?mode=ro', uri=True) as connection:
            return connection.execute('SELECT * FROM messages WHERE presence=0 ORDER BY seq').fetchall()

    retained = history()
    evidence = {'initial_min_requests': initial, 'toplevel': top,
                'owned_output_before': output, 'owned_output_tall': gui.display_geometry(),
                'style': 'irc', 'glyph_string': word, 'phases': []}

    def measure(label):
        gui.framed_keys(w, 'Home')
        start = gui._composer_native(w, word, 0, 0)
        gui.framed_keys(w, 'End')
        finish = gui._composer_native(w, word, end, end)
        caret = finish['caret']
        words = _policy_words(ctx, 'scale-' + label)
        image = gui.native_rgb_capture(w)
        rectangle = gui.read_layout(prefs)['conversation']['rect']
        pairs = [gui.native_marker_components(image, rectangle, rgb)
                 for rgb in (color, (179, 79, 213))]
        assert all(items and all(item['full'] for item in items) for items in pairs), pairs
        marker, right = [max((item['bbox'] for item in items), key=lambda box: box[1])
                         for items in pairs]
        assert marker[2] < right[0] and abs(marker[1] - right[1]) <= 1, pairs

        def glyph_boxes(region, suffix):
            rows = upload.visible_words(w, Path(ctx['destination']), 'scale-' + label + '-' + suffix,
                                        region=region, psm=7)
            return [(int(row['left']), int(row['top']),
                     int(row['left']) + int(row['width']), int(row['top']) + int(row['height']))
                    for row in rows if row['text'].casefold() == word.casefold()]

        # Crop only the actual insertion span/body between colored anchors;
        # Input borders otherwise get recognized as '=' around the real word.
        composer = glyph_boxes((start['caret'][0], caret[1],
                                caret[0] - start['caret'][0], caret[3]), 'input-glyphs')
        bodies = glyph_boxes((marker[2], marker[1] - 1,
                              right[0] - marker[2], marker[3] - marker[1] + 2), 'body-glyphs')
        chrome = [box for text, box in words if text.casefold() == 'blastirc']
        assert len(composer) == len(bodies) == len(chrome) == 1, (words, marker, caret)
        body, entry = bodies[0], composer[0]
        dims = lambda box: (box[2] - box[0], box[3] - box[1])
        assert all(abs(a - b) <= 2 for a, b in zip(dims(body), dims(entry))), (
            'Active dense body and composer differ for the identical native glyph string', body, entry)
        advance = finish['caret'][0] - start['caret'][0]
        assert advance > 0 and start['caret'][1] == finish['caret'][1], (start, finish)
        current = requests()
        assert current and current[-1][0] == 720 and current[-1][1] >= 480, current
        assert all(a != b for a, b in zip(current, current[1:])), current
        result = {'label': label, 'saved_scales': {key: tomllib.loads(prefs.read_text())['appearance'][key]
                  for key in ('scale', 'message_scale')}, 'body_bbox': body, 'composer_bbox': entry,
                  'body_size': dims(body), 'composer_size': dims(entry), 'chrome_bbox': chrome[0],
                  'chrome_size': dims(chrome[0]), 'input_start': start, 'input_end': finish,
                  'input_advance': advance, 'minimum': current[-1], 'min_request_count': len(current),
                  'native_geometry': gui.window_geometry(w), 'marker_bbox': marker}
        before = len(current)
        gui.framed_keys(w, 'Left', 'Right', 'Home', 'End')
        gui._composer_native(w, word, end, end)
        gui.native_rgb_capture(w)
        assert len(requests()) == before, 'Unchanged native frames repeated minimum requests'
        assert history() == retained, 'Scale transition changed retained non-presence history'
        evidence['phases'].append(result)
        return result

    def focus_input(expected):
        # Settings opened from the gear correctly returns focus to that gear.
        # Re-select the same real conversation to focus its editor without
        # moving/collapsing the preserved native draft selection.
        focus_offset = debug.stat().st_size
        network, target = ctx['query_id'].split('/', 1)
        gui.switch_buffer(target, network)
        state = gui._composer_native(w, *expected)
        assert re.search(r'zwp_text_input_v3[@#]\d+\.enable\(\)',
                         debug.read_bytes()[focus_offset:].decode(errors='replace')), state
        return state

    def settings(interface=None, message=None, expected=None):
        prior = tomllib.loads(prefs.read_text())['appearance']['scale']
        gui.open_settings(w, prefs, from_icon=True)
        gui.framed_keys(w, 'shift+Tab')
        gui.settings_category(w, prior, 0)
        for stop, target in ((4, interface), (5, message)):
            if target is None:
                continue
            gui.framed_keys(w, *(['Tab'] * stop))
            # Native slider Home plus declared keyboard steps; no pointer guesses.
            steps = round((target - .7) / .01)
            gui.framed_keys(w, 'Home', *(['Page_Up'] * (steps // 10)), *(['Right'] * (steps % 10)))
            gui.framed_keys(w, *(['shift+Tab'] * stop))
        saved = gui.save_settings_footer(w, saved_path=prefs)
        for key, value in (('scale', interface), ('message_scale', message)):
            if value is not None:
                assert saved['appearance'][key] == value, (key, value, saved['appearance'])
                assert tomllib.loads(prefs.read_text())['appearance'][key] == value
        gui.close_settings_navigation(w)
        gui.wait_layout(p, prefs, predicate=lambda layout: 'settings' not in layout)
        if expected is not None:
            focus_input(expected)
        return saved['appearance']

    baseline = measure('i1-m1')
    settings(interface=1.5, expected=(word, end, end))
    interface = measure('i1.5-m1')
    for key in ('body_size', 'composer_size'):
        assert all(abs(a - b) <= 2 for a, b in zip(baseline[key], interface[key])), (key, baseline, interface)
    assert abs(baseline['input_advance'] - interface['input_advance']) <= 1
    assert all(b > a for a, b in zip(baseline['chrome_size'], interface['chrome_size'])), evidence
    assert interface['minimum'][1] > baseline['minimum'][1], evidence
    settings(message=1.5, expected=(word, end, end))
    message = measure('i1.5-m1.5')
    for key in ('body_size', 'composer_size'):
        assert all(b > a for a, b in zip(interface[key], message[key])), (key, interface, message)
    assert message['input_advance'] > interface['input_advance']
    assert all(abs(a - b) <= 1 for a, b in zip(interface['chrome_size'], message['chrome_size'])), evidence
    assert message['minimum'][1] >= interface['minimum'][1], evidence

    # Preserve exact raw bytes, native byte selection and actual undo history.
    gui.clear_draft()
    raw = '\x02Scale raw café e\u0301 猫🙂\x0f'
    gui.seed_clipboard(raw, ctx['cleanup'])
    gui.framed_keys(w, 'ctrl+v')
    raw_end = len(raw.encode('utf-8'))
    gui._composer_native(w, raw, raw_end, raw_end)
    gui.framed_keys(w, 'ctrl+Home', 'shift+Right')
    selected = gui.native_input_state(w)
    assert selected['text'] == raw and selected['cursor'] != selected['anchor'], selected
    selection = (raw, selected['cursor'], selected['anchor'])
    settings(interface=2, expected=selection)
    gui.framed_keys(w, 'ctrl+slash')
    undone = gui.native_input_state(w)
    assert undone is not None and undone['text'] != raw, ('Settings lost native undo history', undone)
    gui.command('redo', window=w)
    gui.framed_keys(w, 'ctrl+End')
    gui._composer_native(w, raw, raw_end, raw_end)
    gui.framed_keys(w, 'ctrl+Home', 'shift+Right')
    gui._composer_native(w, *selection)
    evidence['raw_selection_preserved'] = selected
    evidence['undo_state'] = undone

    # Genuine input-method preedit is cancelled by Settings focus loss, never
    # committed into the durable/raw draft; reopening Input must reactivate IME.
    with nullcontext(ctx['input_method']) as method:
        gui.SESSION.until('scale probe genuine IME activation', lambda: (
            method.drain() is None and method.state['active']))
        mark = debug.stat().st_size
        serial = method.transaction('ScalePreedit', 12, 12)
        preedit = re.compile(r'zwp_text_input_v3[@#]\d+\.preedit_string\("ScalePreedit",\s*12,\s*12\)')
        gui.SESSION.until('real scale probe preedit delivered', lambda: preedit.search(
            debug.read_bytes()[mark:].decode(errors='replace')))
        gui.await_native_frame(w, mark, preedit)
        gui._composer_native(w, *selection)
        preedit_capture = str(_policy_capture(ctx, 'scale-genuine-preedit'))
        trace_start = len(method.trace)
        settings(message=2, expected=selection)
        gui.SESSION.until('scale probe IME restored after Settings', lambda: (
            method.drain() is None and method.state['active'] and
            method.state['surrounding'] == selection))
        transition_trace = method.trace[trace_start:]
        assert any(item['event'] == 'deactivate' for item in transition_trace), transition_trace
        after = debug.read_bytes()[mark:].decode(errors='replace')
        assert not re.search(r'zwp_text_input_v3[@#]\d+\.commit_string\("ScalePreedit"\)', after)
        evidence['ime'] = {'serial': serial, 'preedit_capture': preedit_capture,
                           'trace': list(transition_trace), 'raw_after': gui.native_input_state(w),
                           'focus_loss_cancels_preedit_without_commit': True}

    declared = requests()[-1]
    assert declared[0] == 720 and declared[1] >= 960, declared
    assert declared[1] >= message['minimum'][1], (declared, message)
    # Ask for exactly the actual advertised minimum, never a below-min clipping case.
    gui.SESSION.ipc('command', f'[con_id={w}] floating enable, resize set {declared[0]} px {declared[1]} px, move position center')
    gui.SESSION.until('actual advertised minimum client allocation', lambda:
        (gui.window_geometry(w)['WIDTH'], gui.window_geometry(w)['HEIGHT']) == declared)
    gui.open_settings(w, prefs, from_icon=True)
    minimum_words = _policy_words(ctx, 'scale-declared-minimum-settings')
    save = [box for text, box in minimum_words if text.casefold() == 'save']
    assert save, ('Settings Save inaccessible at declared minimum', declared, minimum_words)
    bounds = gui.window_geometry(w)
    assert all(0 <= box[0] < box[2] <= bounds['WIDTH'] and
               0 <= box[1] < box[3] <= bounds['HEIGHT'] for box in save), (save, bounds)
    gui.framed_keys(w, 'shift+Tab')
    saved = gui.save_settings_footer(w, saved_path=prefs)
    assert saved['appearance']['scale'] == saved['appearance']['message_scale'] == 2
    gui.close_settings_navigation(w)
    focus_input(selection)
    assert history() == retained
    evidence['declared_minimum_fit'] = {'minimum': declared, 'geometry': bounds, 'save_boxes': save,
                                       'saved_scales': [2, 2]}
    evidence['all_min_requests'] = requests()
    assert all(a != b for a, b in zip(requests(), requests()[1:])), requests()
    gui.SESSION.ipc('command', f'[con_id={w}] floating disable')
    settings(interface=1, message=1, expected=selection)
    gui.clear_draft()
    gui.resize_panel_viewport(w, output)
    evidence['restored_scales'] = [1, 1]
    return evidence


def run_message_links(ctx):
    """Real transcript/menu keys, private exec capture, cancellation and IME guards."""
    import os
    import sys

    destination = Path(ctx['destination'])
    private = destination.with_name(destination.stem + '-message-link-exec')
    private.mkdir(mode=0o700, exist_ok=False)
    calls = private / 'calls.jsonl'
    opener = private / 'xdg-open'
    # This is the actual posix_spawnp boundary, not a replacement UI/controller.
    opener.write_text('#!' + sys.executable + '\n'
        'import json, os, sys\n'
        'fd = os.open(' + repr(str(calls)) + ', os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)\n'
        'os.write(fd, (json.dumps(sys.argv[1:]) + "\\n").encode())\n'
        'os.fsync(fd)\n'
        'os.close(fd)\n')
    opener.chmod(0o700)
    previous_env = dict(ctx['env'])
    ctx['env'] = dict(previous_env, PATH=str(private) + os.pathsep + previous_env['PATH'])
    ctx['cleanup'].callback(lambda: ctx.update(env=previous_env))
    previews = tomllib.loads(Path(ctx['preferences']).read_text())['previews']
    previews.update(links='off', images='off', media='off')
    _refresh_restart(ctx, 'previews', {key: json.dumps(value) for key, value in previews.items()})
    p, w, prefs, db = ctx['proc'], ctx['window'], ctx['preferences'], ctx['database_path']
    _policy_focus(ctx, ctx['query_id'])
    trace = gui.GUI_DEBUG[w][1]
    report = {'popups': [], 'captures': [], 'exec_boundary': str(calls), 'status': []}

    def launched():
        return [json.loads(line) for line in calls.read_text().splitlines()] if calls.exists() else []

    def barrier(label):
        token = 'native-menu-' + label + '-' + os.urandom(5).hex()
        ctx['observer'].send('PING :' + token)
        ctx['observer'].wait(lambda command, params, prefix:
            command == 'PONG' and token in params, timeout=15)

    barrier('start')
    wire_start = len(ctx['observer'].frames)
    expected_launches = []

    def exact_launches():
        assert launched() == [[url] for url in expected_launches], (launched(), expected_launches)

    def retained():
        gui.wait_messages(p, db, ctx['expected_messages'])

    def selected(*keys):
        gui.framed_keys(w, *keys)
        gui.SESSION.until('dedicated native transcript navigation focus',
                          lambda: gui.native_input_state(w) is None)

    def alive(popup):
        return not re.search(rf"xdg_popup[@#]{popup['popup']}\.destroy\(",
                             trace.read_bytes()[popup['creation_offset']:].decode(errors='replace'))

    def capture(popup, label):
        report['popups'].append(dict(popup, label=label))
        report['captures'].append(str(gui.capture_native_popup(w, popup, destination, label)))

    def choose(popup, url, *keys):
        gui.popup_keys(w, popup, *keys, 'Return', root_changed=False)
        expected_launches.append(url)
        gui.SESSION.until('exact actual xdg-open execution',
                          lambda: launched() == [[item] for item in expected_launches])
        exact_launches()

    origin = ctx['cleanup'].enter_context(_RefreshMediaOrigin(ctx['lab'], {
        '/one': (b'<html>One</html>', 'text/html'),
        '/two': (b'<html>Two</html>', 'text/html'),
        '/single': (b'<html>Single</html>', 'text/html')}))
    urls = [origin.url + suffix for suffix in ('/one', '/two', '/single')]
    raw = ('MenuFormatted ' + '\x02' + urls[0] + '\x02 ' + '\x1d' + urls[1] +
           '\x1d duplicate ' + urls[0])
    _policy_send(ctx, raw)
    _policy_send(ctx, 'MenuNoURL café e\u0301 猫')
    draft = 'Keep \x02raw\x02 café e\u0301 猫🙂'
    gui.seed_clipboard(draft, ctx['cleanup'])
    gui.framed_keys(w, 'ctrl+v')
    draft_bytes = len(draft.encode('utf-8'))
    gui._composer_native(w, draft, draft_bytes, draft_bytes)
    gui.framed_keys(w, 'ctrl+Home', 'shift+Right', 'shift+Right')
    original = gui._composer_native(w, draft, 2, 0)
    # Newest is the no-link row; C-p reaches the formatted row and C-n returns.
    selected('ctrl+p', 'ctrl+p', 'ctrl+n')
    no_link_offset = trace.stat().st_size
    gui.key('Return', trace=True)
    gui.key('ctrl+g')
    gui._composer_native(w, draft, 2, 0)
    barrier('zero-link')
    assert not re.search(rb'get_popup\(new id xdg_popup', trace.read_bytes()[no_link_offset:])
    exact_launches()
    retained()
    report['zero_link'] = {'popup_count': 0, 'exec_count': 0, 'raw_draft': draft}

    selected('ctrl+p', 'ctrl+p')
    opening = trace.stat().st_size
    # Keep the same physical Enter pressed through the compositor repeat delay.
    repeat = list(re.finditer(r'wl_keyboard[@#]\d+\.repeat_info\((\d+),\s*(\d+)\)',
                              trace.read_text(errors='replace')))
    assert repeat and int(repeat[-1][1]) > 0, 'Native repeat configuration missing'
    rate, delay = map(int, repeat[-1].groups())
    hold_ms = max(2500, delay + 1500)
    held = subprocess.Popen(['wtype', '-P', 'Return', '-s', str(hold_ms), '-p', 'Return'],
                            env=gui.SESSION.env | {'WAYLAND_DEBUG': 'client'},
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, start_new_session=True)
    ctx['cleanup'].callback(gui.stop_group, held)
    popup = gui.await_native_popup(w, opening)
    assert held.poll() is None, 'Opening Enter was not held while menu mapped'
    time.sleep((delay + 3 * 1000 / rate) / 1000)
    assert held.poll() is None and alive(popup), 'Held opening Enter activated/closed chooser'
    exact_launches()
    _, errors = held.communicate(timeout=15)
    assert held.returncode == 0, errors.decode(errors='replace')
    pressed_codes = re.findall(rb'zwp_virtual_keyboard_v1[@#]\d+\.key\(\d+,\s*(\d+),\s*1\)', errors)
    assert len(pressed_codes) == 1, errors.decode(errors='replace')
    opening_code = int(pressed_codes[0])
    gui.SESSION.until('opening Enter release reaches real application', lambda:
        re.search(rf'wl_keyboard[@#]\d+\.key\(\d+,\s*\d+,\s*{opening_code},\s*0\)'.encode(),
                  trace.read_bytes()[opening:]))
    assert alive(popup), 'Opening Enter release closed chooser'
    exact_launches()
    capture(popup, 'message-links-held-opening-enter')
    report['opening_enter'] = {'repeat_rate': rate, 'repeat_delay_ms': delay,
                              'held_ms': hold_ms, 'exec_before_explicit_enter': 0}
    # Separate activations make either dropped direction fail exact URL capture.
    choose(popup, urls[1], 'Home', 'ctrl+n')
    gui.key('ctrl+g')
    assert gui._composer_native(w, draft, 2, 0) == original
    selected('ctrl+p', 'ctrl+p')
    popup = gui.open_native_popup(w, lambda: gui.key('Return'))
    capture(popup, 'message-links-root-reverse')
    choose(popup, urls[0], 'End', 'ctrl+p')
    gui.key('ctrl+g')
    assert gui._composer_native(w, draft, 2, 0) == original

    _policy_send(ctx, 'MenuSingle ' + urls[2])
    selected('ctrl+p')
    popup = gui.open_native_popup(w, lambda: gui.key('Return'))
    capture(popup, 'message-links-single-still-menu')
    exact_launches()
    choose(popup, urls[2], 'Home')
    gui.key('ctrl+g')
    gui._composer_native(w, draft, 2, 0)

    def status_words(label):
        words = _policy_words(ctx, 'message-status-' + label)
        connected = [box for word, box in words if word.casefold() == 'connected']
        assert connected, ('No actual structural connection glyphs', words)
        band = max(connected, key=lambda box: box[1])
        line = [(word, box) for word, box in words
                if box[1] < band[3] and box[3] > band[1]]
        report['status'].append({'phase': label, 'words': line})
        return line

    def status_text(words):
        return re.sub(r'\s+', '', ''.join(word for word, box in words)).casefold()
    # The earlier menu phase intentionally preserves its selection. Status
    # clearing is independent of C-g's existing selection-cancel behavior.
    gui.framed_keys(w, 'ctrl+End')
    status_native = gui._composer_native(w, draft, draft_bytes, draft_bytes)
    status_edit = (status_native['text'], status_native['cursor'], status_native['anchor'])
    report['status_native_baseline'] = status_native
    gui.framed_keys(w, 'ctrl+g')
    report['status_native_clean'] = gui._composer_native(w, *status_edit)
    structural = status_words('clean-baseline')
    profiles = tomllib.loads(Path(prefs).with_name('config.toml').read_text())['server']
    server_name = ctx['query_id'].split('/', 1)[0]
    profile = next(item for item in profiles if item['name'] == server_name)
    network_name = profile.get('alias') or server_name
    for indicator in (ctx['own_nick'], network_name, 'Connected'):
        assert re.sub(r'\s+', '', indicator).casefold() in status_text(structural), (indicator, structural)
    empty_status = status_text(structural)

    # Status is produced by a real invalid message action, not a model hook.
    gui.framed_keys(w, 'ctrl+r')
    gui._composer_native(w, *status_edit)
    normal = status_words('normal-before')
    assert status_text(normal) != empty_status, ('Real invalid action produced no visible transient feedback', normal)
    gui.framed_keys(w, 'ctrl+g')
    report['status_native_after_normal_cancel'] = gui._composer_native(w, *status_edit)
    clean = status_words('normal-after')
    assert status_text(clean) == empty_status, ('C-g did not restore the baseline status field', structural, clean)

    gui.framed_keys(w, 'ctrl+r')
    gui._composer_native(w, *status_edit)
    produced = status_words('menu-status-produced')
    assert status_text(produced) != empty_status, ('Menu status producer gave no visible feedback', produced)
    popup = gui.open_native_popup(w, lambda: gui.key('Menu'))
    capture(popup, 'message-status-open-context')
    gui.popup_keys(w, popup, 'ctrl+g')
    report['status_native_after_menu_cancel'] = gui._composer_native(w, *status_edit)
    cleared = status_words('menu-after')
    assert status_text(cleared) == empty_status, ('Menu C-g changed structural nick/network/connection fields', structural, cleared)

    # Existing native context menu proves forwarding to the deepest live child.
    report['submenu'] = []
    for label, navigation in (('next', ('Home', 'ctrl+n')),
                              ('previous', ('Home', 'Down', 'Down', 'ctrl+p'))):
        gui.clear_draft()
        text = 'Native context selection ' + label
        gui.type_text(text)
        gui.framed_keys(w, 'ctrl+Home', 'ctrl+shift+End')
        end = len(text.encode('utf-8'))
        gui._composer_native(w, text, end, 0)
        popup = gui.open_native_popup(w, lambda: gui.key('Menu'))
        menu_words = _policy_words(ctx, 'message-existing-context-labels-' + label, popup['rect'])
        spelling = any(word.casefold() == 'spelling' for word, box in menu_words)
        child = gui.open_native_submenu(w, popup, 2 + int(spelling), from_end=True)
        capture(child, 'message-existing-formatting-child-' + label)
        gui.popup_keys(w, popup, *navigation, 'Return')
        payload = '\x1d' + text + '\x1d'
        gui._composer_native(w, payload, len(payload.encode('utf-8')), len(payload.encode('utf-8')))
        report['submenu'].append({'direction': label, 'root': popup, 'child': child,
                                  'exact_raw_payload': payload})
    # Ordinary composer Enter remains a real IRC send, even after menu use.
    gui.framed_keys(w, 'Return')
    ctx['receive']('PRIVMSG', [ctx['query_target'], payload], ctx['own_nick'])
    frame = ctx['observer'].frames[-1]
    assert frame['command'] == 'PRIVMSG' and frame['params'] == [ctx['query_target'], payload], frame
    ctx['expected_messages'].append((ctx['query_id'], ctx['own_nick'], payload, 1, 0))
    retained()
    report['ordinary_composer_enter_wire'] = frame
    gui._composer_native(w, '', 0, 0)
    expected_own_frames = [frame]
    report['opening_key_lifecycle'] = []

    def native_key(code, pressed):
        # Linux evdev KEY_ENTER=28 / KEY_ESC=1 in VirtualSeat's published us-intl map.
        seat = gui.POINTER
        seat.request(seat.keyboard, 1, [
            seat.timestamp(), gui._WaylandArgument(u=code), gui._WaylandArgument(u=int(pressed))])
        seat.roundtrip()

    def native_enter():
        native_key(28, True)
        native_key(28, False)

    # Dismissing while the opening key is down must retire Ui's consumed-key state.
    _policy_send(ctx, 'HeldDismiss ' + urls[0])
    lifecycle_draft = 'Deliberate Enter after held menu dismissal'
    gui.type_text(lifecycle_draft)
    selected('ctrl+p')
    gui.POINTER.publish_keymap()
    opened_at = trace.stat().st_size
    native_key(28, True)
    dismissed = gui.await_native_popup(w, opened_at)
    native_key(1, True)
    native_key(1, False)
    gui.SESSION.until('held-Enter chooser dismissed by native Escape', lambda: not alive(dismissed))
    released_at = trace.stat().st_size
    native_key(28, False)
    gui.SESSION.until('same physical opening Enter released after dismissal', lambda:
        re.search(rb'wl_keyboard[@#]\d+\.key\(\d+,\s*\d+,\s*28,\s*0\)',
                  trace.read_bytes()[released_at:]))
    exact_launches()
    gui.framed_keys(w, 'ctrl+g')
    size = len(lifecycle_draft.encode('utf-8'))
    gui._composer_native(w, lifecycle_draft, size, size)
    gui.POINTER.publish_keymap()
    native_enter()
    ctx['receive']('PRIVMSG', [ctx['query_target'], lifecycle_draft], ctx['own_nick'])
    lifecycle_frame = ctx['observer'].frames[-1]
    assert (lifecycle_frame['command'], lifecycle_frame['params']) == (
        'PRIVMSG', [ctx['query_target'], lifecycle_draft]), lifecycle_frame
    expected_own_frames.append(lifecycle_frame)
    ctx['expected_messages'].append((ctx['query_id'], ctx['own_nick'], lifecycle_draft, 1, 0))
    retained()
    gui._composer_native(w, '', 0, 0)
    exact_launches()
    report['opening_key_lifecycle'].append({
        'case': 'held-dismiss-release', 'popup': dismissed, 'opening_raw_key': 28,
        'deliberate_composer_enter_wire': lifecycle_frame})

    # A separate real GTK client receives the release that BlastIRC must not see.
    portal, backend = (ctx['env'].get(name) for name in
                       ('BLASTIRC_PORTAL', 'BLASTIRC_PORTAL_BACKEND'))
    assert portal and backend, 'Native external-focus proof requires pinned private GTK portal binaries'
    portal_output = private / 'external-native-portal'
    portal_output.mkdir(mode=0o700)
    services = ctx['cleanup'].enter_context(upload.DesktopServices(
        dict(ctx['env']), portal_output, Path(portal), Path(backend)))
    services.start_portals()
    request = subprocess.Popen([
        'gdbus', 'call', '--session', '--dest', 'org.freedesktop.impl.portal.desktop.gtk',
        '--object-path', '/org/freedesktop/portal/desktop', '--method',
        'org.freedesktop.impl.portal.FileChooser.OpenFile',
        '/org/freedesktop/portal/desktop/request/native_menu_probe',
        'org.blastirc.NativeMenuProbe', '', 'Choose a file', "{'modal': <false>}"],
        env=services.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
    ctx['cleanup'].callback(gui.stop_group, request)

    def external_window():
        candidates = [node for node, _ in gui.tree_windows(gui.SESSION.ipc('get_tree'))
                      if node.get('pid') == services.backend_process.pid and
                      node.get('shell') == 'xdg_shell' and node.get('name') == 'Choose a file']
        assert len(candidates) <= 1, candidates
        if request.poll() is not None and not candidates:
            output, errors = request.communicate()
            raise AssertionError(('Independent native GTK chooser failed', output, errors))
        return candidates[0] if candidates else None

    external = gui.SESSION.until('owned independent GTK portal surface', external_window)
    external_id = external['id']
    gui.SESSION.ipc('command', f'[con_id={external_id}] floating enable')
    gui.SESSION.ipc('command', f'[con_id={w}] focus')
    gui.SESSION.until('BlastIRC focus restored before lost-release setup',
                      lambda: gui.native_window(w).get('focused'))
    _policy_send(ctx, 'LostRelease ' + urls[1])
    selected('ctrl+p')
    gui.POINTER.publish_keymap()
    opened_at = trace.stat().st_size
    native_key(28, True)
    lost = gui.await_native_popup(w, opened_at)
    focus_away = trace.stat().st_size
    gui.SESSION.ipc('command', f'[con_id={external_id}] focus')
    gui.SESSION.until('real keyboard leave to owned external native surface', lambda:
        gui.native_window(external_id).get('focused') and
        re.search(rb'wl_keyboard[@#]\d+\.leave\(\d+,\s*wl_surface[@#]\d+',
                  trace.read_bytes()[focus_away:]))
    external_release = trace.stat().st_size
    native_key(28, False)
    assert gui.native_window(external_id).get('focused')
    return_focus = trace.stat().st_size
    gui.SESSION.ipc('command', f'[con_id={w}] focus')
    gui.SESSION.until('real keyboard re-enter after external release', lambda:
        gui.native_window(w).get('focused') and re.search(
            rf"wl_keyboard[@#]\d+\.enter\(\d+,\s*wl_surface[@#](?:{gui.GUI_DEBUG[w][2]}|{lost['surface']}),".encode(),
            trace.read_bytes()[return_focus:]))
    assert not re.search(rb'wl_keyboard[@#]\d+\.key\(\d+,\s*\d+,\s*28,\s*0\)',
                         trace.read_bytes()[external_release:]), (
        'Opening release incorrectly reached BlastIRC instead of external surface')
    exact_launches()
    survives = alive(lost)
    fresh_enter = trace.stat().st_size
    native_enter()
    if not survives:
        # Compositor dismissal is valid: the FIRST fresh press must reopen, not vanish.
        reopened = gui.await_native_popup(w, fresh_enter)
        capture(reopened, 'message-links-lost-release-first-enter-reopens')
        exact_launches()
        native_enter()
    expected_launches.append(urls[1])
    gui.SESSION.until('first deliberate native activation after lost release', lambda:
        launched() == [[item] for item in expected_launches])
    exact_launches()
    report['opening_key_lifecycle'].append({
        'case': 'release-on-external-native-surface', 'popup': lost,
        'external_surface': {key: external.get(key) for key in ('id', 'pid', 'name', 'shell')},
        'opening_raw_key': 28, 'popup_survived_focus_loss': survives,
        'external_release_log_offset': external_release, 'refocus_log_offset': return_focus,
        'exact_explicit_url': urls[1]})
    gui.SESSION.ipc('command', f'[con_id={external_id}] focus')
    native_key(1, True)
    native_key(1, False)
    gui.SESSION.until('owned external chooser cancelled without file selection', lambda:
        all(node['id'] != external_id for node, _ in gui.tree_windows(gui.SESSION.ipc('get_tree'))))
    _, request_errors = request.communicate(timeout=15)
    assert request.returncode == 0, request_errors.decode(errors='replace')
    gui.SESSION.ipc('command', f'[con_id={w}] focus')
    gui.framed_keys(w, 'ctrl+g')
    gui._composer_native(w, '', 0, 0)

    # An active authoritative IME preedit must not be navigated, submitted or lost.
    with gui.nullcontext(ctx['input_method']) as method:
        assert method is not None, 'Fixture requires its existing real input method'
        def active():
            method.drain()
            return method.state['active'] and method.state['surrounding'] == ('', 0, 0)
        gui.SESSION.until('private IME activation for menu guard', active)
        empty_caret = gui._composer_native(w, '', 0, 0)['caret']
        blank = gui.native_rgb_capture(w)
        ime_offset = trace.stat().st_size
        method.transaction('猫', 3, 3)
        gui.await_native_frame(w, ime_offset,
                               re.compile(r'zwp_text_input_v3[@#]\d+\.preedit_string\('))
        preedit = gui._composer_native(w, '', 0, 0)
        left, top, _, height = empty_caret
        right = preedit['caret'][0]
        glyph_box = (round(left + 2), round(top), round(right - 2), round(top + height))
        assert glyph_box[0] < glyph_box[2], ('No painted preedit advance', empty_caret, preedit)
        glyphs = _policy_crop(gui.native_rgb_capture(w), glyph_box)
        assert glyphs != _policy_crop(blank, glyph_box), 'Preedit has no actual glyph raster'
        gui.framed_keys(w, 'ctrl+p', 'ctrl+n', 'Return')
        guarded = gui._composer_native(w, '', 0, 0)
        assert guarded == preedit, ('Message navigation displaced active IME', preedit, guarded)
        assert _policy_crop(gui.native_rgb_capture(w), glyph_box) == glyphs, 'Navigation lost preedit glyphs'
        method.drain()
        assert method.state['active'], method.state
        method.transaction('', 0, 0, committed='猫')
        committed = gui._composer_native(w, '猫', 3, 3)
        assert not re.search(rb'get_popup\(new id xdg_popup', trace.read_bytes()[ime_offset:])
        report['ime'] = {'preedit': '猫', 'navigation_guarded': True, 'committed_native': committed,
                         'preserved_glyph_box': glyph_box, 'preedit_native': preedit}
    gui.clear_draft()
    gui.key('ctrl+g')
    barrier('final')
    own = [item for item in ctx['observer'].frames[wire_start:]
           if item['command'] == 'PRIVMSG' and item['prefix'].split('!', 1)[0] == ctx['own_nick']]
    assert own == expected_own_frames, ('Navigation/menu/IME unexpectedly sent a message', own)
    exact_launches()
    assert origin.snapshot() == [], ('Menu navigation unexpectedly fetched URLs', origin.snapshot())
    retained()
    report.update(exact_exec_arguments=launched(), preview_request_count=0,
                  exact_retained_formatted_body=raw, original_raw_draft=original)
    return _refresh_return(ctx, 'message-links', report)


def run_typing_focus(ctx):
    """Minimal real first-key handoff and registered-prefix priority proof."""
    w = ctx['window']
    _policy_focus(ctx, ctx['query_id'])
    _policy_send(ctx, 'Native typing handoff anchor')
    draft = 'keep café '
    gui.type_text(draft)
    size = len(draft.encode('utf-8'))
    gui._composer_native(w, draft, size, size)
    gui.framed_keys(w, 'ctrl+p')
    gui.SESSION.until('actual transcript keyboard focus before ordinary typing',
                      lambda: gui.native_input_state(w) is None)
    debug = gui.GUI_DEBUG[w][1]
    offset = debug.stat().st_size
    gui.framed_keys(w, 'x')
    typed = draft + 'x'
    end = len(typed.encode('utf-8'))
    handed_off = gui._composer_native(w, typed, end, end)
    assert re.search(r'zwp_text_input_v3[@#]\d+\.enable\(\)',
                     debug.read_bytes()[offset:].decode(errors='replace')), handed_off
    gui.framed_keys(w, 'ctrl+p')
    gui.SESSION.until('nonediting focus before the registered prefix',
                      lambda: gui.native_input_state(w) is None)
    gui._open_native_palette('ctrl+x', 'b', window=w)
    picker = gui._composer_native(w, '', 0, 0)
    gui.framed_keys(w, 'Escape')
    gui.framed_keys(w, 'ctrl+g')
    restored = gui._composer_native(w, typed, end, end)
    return _refresh_return(ctx, 'typing-focus', {
        'before': draft, 'first_scalar': 'x', 'exact_once_native': handed_off,
        'registered_prefix_picker': picker, 'unchanged_draft_after_prefix': restored})


def run_buffer_boundaries(ctx):
    """Default M-</M-> on real loaded rows, with native editor and IME witnesses."""
    import os

    p, w = ctx['proc'], ctx['window']
    prefs, db, identity = ctx['preferences'], ctx['database_path'], ctx['query_id']
    destination = Path(ctx['destination'])
    assert ctx['args'].zoom == 1 and ctx['args'].size == (1440, 940), ctx['args']
    bindings = tomllib.loads(Path(prefs).read_text())['bindings']
    assert bindings['beginning-of-buffer'] == 'M-<' and bindings['end-of-buffer'] == 'M->', bindings
    with sqlite3.connect(f'file:{db}?mode=ro', uri=True) as database:
        assert database.execute('SELECT COUNT(*) FROM messages WHERE buffer=?', (identity,)).fetchone() == (0,), \
            'The focused boundary scope requires its fresh private query'
    nonce = os.urandom(4).hex()
    first_rgb, source_rgb, tail_rgb, follow_rgb = (42, 185, 113), (25, 167, 211), (211, 121, 37), (211, 90, 145)
    first = f'Boundary first \x042AB971,2AB971Astra{nonce}\x0f.'
    _policy_send(ctx, first)
    _policy_focus(ctx, identity)
    for index in range(50):
        _policy_send(ctx, f'Boundary {nonce} loaded row {index:02d}', durable=False)
    source = f'Boundary reply source \x0419A7D3,19A7D3Astra{nonce}\x0f.'
    _policy_send(ctx, source)
    tail = f'Boundary newest \x04D37925,D37925Astra{nonce}\x0f.'
    _policy_send(ctx, tail)
    _policy_wait_box(ctx, tail_rgb)
    source_marker = _policy_wait_box(ctx, source_rgb)
    acknowledged = gui.wait_read_marker(p, db, identity)
    assert acknowledged[3] == tail, acknowledged
    layout = gui.read_layout(prefs)
    reply = gui.begin_native_reply(w, layout, 1, destination, label='boundary-reply')
    draft = 'Boundary café draft'
    gui.type_text(draft)
    size = len(draft.encode('utf-8'))
    gui.framed_keys(w, 'ctrl+Home', 'ctrl+shift+End')
    before = gui._composer_native(w, draft, size, 0)
    # Reply is an unreserved transparent overlay: the newest body can overlap
    # its heading. Keep the quote's cyan identity separate from both endpoints,
    # and measure its actual swatch instead of OCR-ing overlapping heading text.
    baseline = gui.native_rgb_capture(w)
    swatches = [item for item in gui.native_marker_components(
        baseline, layout['conversation']['rect'], source_rgb)
        if item['full'] and item['bbox'][1] > source_marker[3]]
    assert swatches, ('No actual native reply quote below its original source', source_marker, swatches)
    quote_swatch = max(swatches, key=lambda item: item['bbox'][1])
    qx, qy, qr, qb = quote_swatch['bbox']
    excerpt = (qx - 1, qy - 1, qr - qx + 2, qb - qy + 2)
    ex, ey, ew, eh = excerpt
    # Endpoint colors never occur in the quote, so sample the full real pane;
    # newest rows are allowed to occupy the active reply card's band.
    transcript_sample = tuple(layout['conversation']['rect'])
    quote_relative_box = [quote_swatch['bbox'][0] - ex, quote_swatch['bbox'][1] - ey,
                          quote_swatch['bbox'][2] - ex, quote_swatch['bbox'][3] - ey]
    reply_observations = []

    def transcript_components(image, rgb):
        return gui.native_marker_components(image, transcript_sample, rgb)

    def transcript_box(rgb):
        components = transcript_components(gui.native_rgb_capture(w), rgb)
        return components[0]['bbox'] if len(components) == 1 and components[0]['full'] else None
    debug = gui.GUI_DEBUG[w][1]
    log_base = debug.stat().st_size
    phases, captures = [], list(reply['captures'])

    def unchanged_editor():
        state = gui._composer_native(w, draft, size, 0)
        assert state == before, ('Boundary changed native draft/selection/focus', before, state)
        image = gui.native_rgb_capture(w)
        assert (image['width'], image['height']) == (baseline['width'], baseline['height'])
        # Compare only the quote's opaque swatch, not the transparent card background.
        assert gui.native_marker_components(image, excerpt, source_rgb) == [quote_swatch], \
            'Boundary changed the actual reply quote identity/geometry'
        reply_observations.append({'native': state, 'quote_swatch': quote_swatch,
                                   'quote_relative_box': quote_relative_box})
        return state

    def boundary(newest, label):
        action = gui.framed_keys(w, 'alt+greater' if newest else 'alt+less')
        rgb, absent = (tail_rgb, first_rgb) if newest else (first_rgb, tail_rgb)
        previous = None

        def painted():
            nonlocal previous
            image = gui.native_rgb_capture(w, capture_timing=True)
            components = transcript_components(image, rgb)
            box = components[0]['bbox'] if len(components) == 1 and components[0]['full'] else None
            if not box or transcript_components(image, absent):
                previous = None
                return None
            stable = box == previous
            previous = box
            return (box, image['capture_timestamps']) if stable else None

        box, frames = gui.SESSION.until('actual default boundary reveals only its loaded endpoint', painted)
        unchanged_editor()
        capture = str(_policy_capture(ctx, 'buffer-boundary-' + label))
        captures.append(capture)
        phases.append({'label': label, 'keys': 'M->' if newest else 'M-<',
                       'trace_offset': action, 'marker_box': box, 'presentation_frames': frames,
                       'capture': capture})
        return box

    with sqlite3.connect(f'file:{db}?mode=ro', uri=True) as database:
        def retained():
            return database.execute('SELECT seq,id,text FROM messages WHERE buffer=? ORDER BY seq',
                                    (identity,)).fetchall()

        loaded = retained()
        assert len(loaded) == 53 and loaded[0][2] == first and loaded[-2][2] == source and loaded[-1][2] == tail, loaded
        first_box = boundary(False, 'first-loaded')
        # Repeating at the boundary must not expose older rows or change its geometry.
        assert boundary(False, 'first-loaded-repeat') == first_box
        assert retained() == loaded, 'Beginning changed the loaded real-wire history'
        tail_box = boundary(True, 'newest')
        assert retained() == loaded, 'End changed the loaded real-wire history'

    # The real private input-method owns preedit; consumed chords need not paint.
    method = ctx['input_method']
    assert method is not None, 'buffer-boundaries requires the standard private input-method fixture'
    method.drain()
    assert method.state['active'], method.state

    def preedit(text):
        offset = debug.stat().st_size
        serial = method.transaction(text, len(text), len(text))
        event = re.compile(r'zwp_text_input_v3[@#]\d+\.preedit_string\("' + re.escape(text) + r'",')
        gui.SESSION.until('actual application boundary preedit delivery', lambda:
            event.search(debug.read_bytes()[offset:].decode(errors='replace')))
        gui.await_native_frame(w, offset, event)
        return serial

    ime_serial = preedit('boundary-ime')
    ime_state = gui.native_input_state(w)
    ime_capture = str(_policy_capture(ctx, 'buffer-boundary-ime-before'))
    cx, cy, cw, ch = ime_state['caret']
    metrics = gui.pane_metrics(gui.read_layout(prefs), 1)
    ime_bounds = (round(metrics['x'] + metrics['left'] + 8), round(cy + ch * .15),
                  round(cx - 2), round(cy + ch * .7))
    assert ime_bounds[0] < ime_bounds[2] and ime_bounds[1] < ime_bounds[3], ime_bounds
    ime_pixels = _policy_crop(gui.native_rgb_capture(w), ime_bounds)
    for chord in ('alt+less', 'alt+greater'):
        offset = debug.stat().st_size
        presses = gui.key(chord, trace=True)
        release = re.compile(rf'wl_keyboard[@#]\d+\.key\(\d+,\s*\d+,\s*{presses[-1]},\s*0\)')
        gui.SESSION.until('real consumed boundary chord release during IME', lambda:
            release.search(debug.read_bytes()[offset:].decode(errors='replace')))
        # Plus hover forces an actual paint without changing draft selection or focus.
        targets = gui.composer_primary_targets(gui.read_layout(prefs), 1, ime_state['caret'])
        gui.move_pointer(w, *targets['send'])
        paint = debug.stat().st_size
        gui.move_pointer(w, *targets['plus'])
        gui.await_native_frame(w, paint, re.compile(r'wl_pointer[@#]\d+\.motion\('))
        assert gui.native_input_state(w) == ime_state, 'Boundary chord changed native IME state'
        assert _policy_crop(gui.native_rgb_capture(w), ime_bounds) == ime_pixels, \
            'Boundary chord changed the actually painted preedit glyph band'
        assert gui.SESSION.until('native transcript tail during IME', lambda: transcript_box(tail_rgb)) == tail_box, \
            'IME boundary chord moved the transcript'
        assert not transcript_box(first_rgb), 'IME boundary chord exposed first row'
    ime_after = str(_policy_capture(ctx, 'buffer-boundary-ime-after'))
    preedit('')
    gui.move_pointer(w, 0, 0)
    unchanged_editor()
    follow = f'Boundary follow \x04D35A91,D35A91Astra{nonce}\x0f.'
    _policy_send(ctx, follow)
    previous_follow = None
    def following_component():
        nonlocal previous_follow
        components = transcript_components(gui.native_rgb_capture(w), follow_rgb)
        # The transparent reply heading can cut into the latest swatch. Require
        # a real unique, stable RGB component, not a fully rectangular fill.
        if len(components) != 1:
            previous_follow = None
            return False
        component = components[0]
        left, top, right, bottom = component['bbox']
        if right - left < 24 or bottom - top < 6 or component['pixels'] <= 0:
            previous_follow = None
            return False
        stable = component == previous_follow
        previous_follow = component
        return component if stable else False
    followed = gui.SESSION.until('native transcript following visible stable arrival', following_component)
    read = gui.wait_read_marker(p, db, identity)
    assert read[3] == follow, read
    unchanged_editor()
    captures.extend((ime_capture, ime_after, str(_policy_capture(ctx, 'buffer-boundary-follow'))))
    trace_path = destination.with_name(destination.stem + '-buffer-boundaries.wayland.log')
    trace_path.write_text(ctx['lab']._redact(debug.read_bytes()[log_base:].decode(errors='replace')))
    cancelled = gui.cancel_native_reply(w, destination, label='boundary-reply-still-active', expected_text=draft)
    return _refresh_return(ctx, 'buffer-boundaries', {
        'loaded_first': loaded[0], 'loaded_newest': loaded[-1], 'loaded_count': len(loaded),
        'phases': phases, 'native_draft_selection': before, 'reply_excerpt_region': excerpt,
        'transcript_marker_sample': transcript_sample,
        'reply_quote_swatch': quote_swatch, 'reply_quote_relative_box': quote_relative_box,
        'reply_source': source, 'reply_source_marker_before': source_marker,
        'reply_preservation_observations': reply_observations,
        'ime_glyph_bounds': ime_bounds,
        'reply_still_active_cancel': cancelled, 'ime_serial': ime_serial,
        'ime_state': ime_state, 'ime_chords': ['M-<', 'M->'],
        'following_marker': followed, 'following_read': read, 'captures': captures,
        'trace': str(trace_path), 'trace_base_offset': log_base,
        'limits': ['Fresh fully loaded query; no remote-only history frontier or fetch-count claim.',
                   'Dialog/menu/control precedence and stale-anchor cancellation remain native routing tests.',
                   'Swatches prove actual endpoint visibility, not arbitrary message glyph fidelity.']})


def run_jump_bottom(ctx):
    """Real zero-arrival departure, native FAB boundary/click, and resumed following."""
    import os

    p, w = ctx['proc'], ctx['window']
    prefs, db, identity = ctx['preferences'], ctx['database_path'], ctx['query_id']
    assert ctx['args'].zoom == 1 and ctx['args'].size == (1440, 940), ctx['args']
    _policy_focus(ctx, identity)
    nonce = os.urandom(4).hex()
    for index in range(50):
        _policy_send(ctx, f'JB{nonce} history {index:02d}', durable=False)
    tail_rgb, follow_rgb = (25, 167, 211), (211, 90, 145)
    # Reuse the rectangular reading-proof swatch; leading J glyphs overhang it.
    tail = f'Jump-bottom tail \x0419A7D3,19A7D3Astra{nonce}\x0f.'
    _policy_send(ctx, tail)
    acknowledged = gui.wait_read_marker(p, db, identity)
    assert acknowledged[3] == tail, acknowledged
    _policy_wait_box(ctx, tail_rgb)
    draft = 'keep café draft'
    gui.type_text(draft)
    size = len(draft.encode('utf-8'))
    before_input = gui._composer_native(w, draft, size, size)
    layout = gui.read_layout(prefs)
    x, y, width, height = layout['conversation']['rect']
    # Short wire bodies leave this strip blank. Exclude the scrollbar gutter;
    # the only new native ink here after departure must be the actual FAB.
    region = (round(x + width - 110), round(y + 100),
              round(x + width - 20), round(before_input['caret'][1]))

    def park_pointer():
        offset = gui.GUI_DEBUG[w][1].stat().st_size
        gui.move_pointer(w, x + width / 2, max(1, y / 2))
        gui.await_native_frame(w, offset, re.compile(r'wl_pointer[@#]\d+\.motion\('))

    def strip():
        image = gui.native_rgb_capture(w)
        assert gui.read_layout(prefs) == layout, 'Native proof geometry changed'
        return Image.frombytes('RGB', (image['width'], image['height']),
                               image['pixels']).crop(region)

    # Match the existing unread proof: exclude software cursor and hover ink.
    park_pointer()
    baseline = strip()
    at_tail = str(_policy_capture(ctx, 'jump-bottom-tail'))
    with sqlite3.connect(f'file:{db}?mode=ro', uri=True) as database:
        def retained():
            return database.execute(
                'SELECT seq,text FROM messages WHERE buffer=? ORDER BY seq', (identity,)).fetchall()

        history = retained()
        assert [row[1] for row in history[-51:-1]] == [
            f'JB{nonce} history {index:02d}' for index in range(50)], history
        gui.move_pointer(w, x + width / 2, y + height / 2)
        offset = gui.GUI_DEBUG[w][1].stat().st_size
        gui.POINTER.scroll(-32)
        gui.await_native_frame(w, offset, re.compile(r'wl_pointer[@#]\d+\.axis\(\d+,\s*0,'))

        def departed():
            image = gui.native_rgb_capture(w)
            if gui.native_marker_components(image, layout['conversation']['rect'], tail_rgb):
                return None
            difference = ImageChops.difference(baseline, strip()).convert('L')
            box = difference.point(lambda value: 255 if value > 24 else 0).getbbox()
            if not box or not (28 <= box[2] - box[0] <= 52 and 28 <= box[3] - box[1] <= 52):
                return None
            return tuple(value + region[index % 2] for index, value in enumerate(box))

        boundary = gui.SESSION.until('native right-side FAB ink with tail offscreen', departed)
        assert boundary[0] > x + width - 100 and boundary[3] < before_input['caret'][1], boundary
        expected_center_x = x + width - gui.pane_metrics(layout, 1)['left'] - 44
        assert abs((boundary[0] + boundary[2]) / 2 - expected_center_x) <= 3, boundary
        away = str(_policy_capture(ctx, 'jump-bottom-away-zero-arrivals'))
        assert retained() == history, 'Messages arrived during the zero-unseen departure'
        assert gui.wait_read_marker(p, db, identity) == acknowledged
        gui._composer_native(w, draft, size, size)
        offset = gui.GUI_DEBUG[w][1].stat().st_size
        point = ((boundary[0] + boundary[2]) / 2, (boundary[1] + boundary[3]) / 2)
        gui.click(w, *point)
        gui.await_native_frame(w, offset,
                               re.compile(r'wl_pointer[@#]\d+\.button\(\d+,\s*\d+,\s*272,\s*0\)'))
        restored_tail = _policy_wait_box(ctx, tail_rgb)
        park_pointer()
        gui.SESSION.until('FAB native ink disappears at restored tail',
                          lambda: ImageChops.difference(baseline, strip()).getbbox() is None)
        after_input = gui._composer_native(w, draft, size, size)
        assert after_input == before_input, ('FAB changed native editor state', before_input, after_input)
        assert retained() == history, 'Messages arrived before the zero-unseen click completed'
        restored = str(_policy_capture(ctx, 'jump-bottom-restored'))

    follow = f'Jump-bottom follow \x04D35A91,D35A91Astra{nonce}\x0f.'
    _policy_send(ctx, follow)
    followed = _policy_wait_box(ctx, follow_rgb)
    read = gui.wait_read_marker(p, db, identity)
    assert read[3] == follow, read
    gui._composer_native(w, draft, size, size)
    return _refresh_return(ctx, 'jump-bottom', {
        'retained_history_rows': len(history), 'no_arrivals_before_click': True,
        'acknowledged_before_departure': acknowledged, 'native_fab_boundary': boundary,
        'actual_pointer_click': point, 'restored_tail_marker': restored_tail,
        'followed_new_wire_marker': followed, 'followed_read_marker': read,
        'unchanged_native_editor': after_input,
        'captures': {'tail': at_tail, 'away_zero_arrivals': away, 'restored': restored,
                     'following': str(_policy_capture(ctx, 'jump-bottom-following'))}})



def run_mentions_unread(ctx):
    """One real-wire unread/read transition, isolated from another conversation."""
    p, w = ctx['proc'], ctx['window']
    prefs, db = ctx['preferences'], ctx['database_path']
    channel, query = ctx['channel_id'], ctx['query_id']
    _policy_focus(ctx, query)
    gui.wait_read_marker(p, db, query)
    _policy_focus(ctx, channel)
    _policy_send(ctx, 'Native unread navigation anchor', target=ctx['channel'])
    acknowledged = gui.wait_read_marker(p, db, channel)
    # Home in the real transcript explicitly leaves tail-follow mode, even
    # when this small fixture's history fits without scrolling.
    gui.framed_keys(w, 'ctrl+p')
    gui.SESSION.until('transcript owns keyboard before Home',
                      lambda: gui.native_input_state(w) is None)
    gui.framed_keys(w, 'Home', 'ctrl+g')
    gui._composer_native(w, '', 0, 0)
    other_body = ctx['own_nick'] + ': Other conversation must stay unread'
    _policy_send(ctx, other_body)
    before = gui.save_state(p, prefs, db)
    other_before = next(row for row in before[2] if row[0] == query)
    assert other_before[1] > 0, ('Other conversation was not genuinely unread', other_before)

    # Locate only the native navigator band, not cosmetic labels or timestamps.
    layout = gui.read_layout(prefs)
    scale = layout['scale']
    x, y, width, height = layout['conversation']['rect']
    metrics = gui.pane_metrics(layout, scale)
    image = gui.native_rgb_capture(w)
    native = Image.frombytes('RGB', (image['width'], image['height']), image['pixels'])
    caret = gui.native_input_rectangle(w)
    column, row = round(x + width - 200 * scale), round(caret[1] + caret[3] / 2)
    canvas = native.getpixel((column, round(y + height / 2)))
    assert native.getpixel((column, row)) != canvas, 'No native composer card boundary'
    while row > y + metrics['top'] and native.getpixel((column, row)) != canvas:
        row -= 1
    bottom = row + 1 - metrics['padding_y'] - metrics['gap']
    right = x + width - metrics['left'] - 26 * scale  # Scroll gutter + overlay margin.
    region = (round(right - 44 * scale), round(bottom - 116 * scale),
              round(44 * scale), round(84 * scale))
    bounds = (region[0], region[1], region[0] + region[2], region[1] + region[3])
    def park_pointer():
        offset = gui.GUI_DEBUG[w][1].stat().st_size
        gui.move_pointer(w, x + width / 2, max(1, y / 2))
        gui.await_native_frame(w, offset, re.compile(r'wl_pointer[@#]\d+\.motion\('))

    # Both captures must exclude the actual software cursor and hover state:
    # the count click otherwise leaves the pointer inside the former pill.
    park_pointer()
    absent = _policy_crop(gui.native_rgb_capture(w), bounds)
    body = '\x042D9BD9,2D9BD9NATIVEUNREAD\x0f ' + ctx['own_nick'] + ': Read only this conversation'
    _policy_send(ctx, body, target=ctx['channel'])
    pending = gui.save_state(p, prefs, db)
    current_before = next(row for row in pending[2] if row[0] == channel)
    assert current_before[1] == 1 and current_before[2] == acknowledged[0], (
        'Nonfollowing real arrival did not retain its unread marker', acknowledged, current_before)
    rows = upload.visible_words(w, Path(ctx['destination']), 'mentions-unread-count',
                                region=region, psm=6)
    counts = [item for item in rows if item['text'].strip() == '1']
    assert len(counts) == 1, ('Native unread navigator must show one eligible mention', rows)
    count = counts[0]
    offset = gui.GUI_DEBUG[w][1].stat().st_size
    gui.click(w, int(count['left']) + int(count['width']) / 2,
              int(count['top']) + int(count['height']) / 2)
    gui.await_native_frame(w, offset,
        re.compile(r'wl_pointer[@#]\d+\.button\(\d+,\s*\d+,\s*272,\s*0\)'))
    read = gui.wait_read_marker(p, db, channel)
    assert read[3] == body and read[0] != acknowledged[0], (acknowledged, read)
    after = gui.save_state(p, prefs, db)
    other_after = next(row for row in after[2] if row[0] == query)
    assert other_after == other_before, ('Count click marked another conversation read', other_before, other_after)
    park_pointer()
    gui.SESSION.until('Core-zero hides the native navigator',
        lambda: _policy_crop(gui.native_rgb_capture(w), bounds) == absent)

    # Reuse the unique wire-painted body marker: a first plain feed-body click
    # must jump to its source without first selecting it or opening a menu.
    gui.command('global-mentions', window=w)
    feed = gui.save_state(p, prefs, db)
    assert feed[1] not in ((channel,), (query,)), feed
    marker = _policy_wait_box(ctx, (45, 155, 217))
    gui.click(w, (marker[0] + marker[2]) / 2, (marker[1] + marker[3]) / 2)
    source = gui.save_state(p, prefs, db)
    assert source[1] == (channel,), ('First feed-body click did not jump to original source', source)
    _policy_wait_box(ctx, (45, 155, 217))
    assert next(row for row in source[2] if row[0] == query) == other_before
    return _refresh_return(ctx, 'mentions-unread', {
        'before_read': current_before, 'core_read': read, 'other_unread_preserved': other_after,
        'native_count': counts, 'navigator_hidden_at_core_zero': True,
        'feed_body_source_jump': list(source[1]),
        'capture': str(_policy_capture(ctx, 'mentions-unread-source-jump'))})


class _StartupHistoryRelay:
    """One private TLS connection; one event-loop thread owns all TLS I/O."""

    def __init__(self, lab, target, endpoint='direct'):
        import asyncio
        import socket
        self.asyncio = asyncio
        self.target = target.casefold()
        self.held = threading.Event()
        self.release = threading.Event()
        self.completed = threading.Event()
        self.stopping = threading.Event()
        self.errors = []
        self.requests = []
        self.tasks = set()
        self.batch = None
        self.loop = None
        self.shutdown = None
        self.listener = socket.socket()
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen(1)
        self.port = self.listener.getsockname()[1]
        self.server_tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.server_tls.load_cert_chain(lab.root / 'server.pem', lab.root / 'server.key')
        self.client_tls = ssl.create_default_context(cafile=lab.ca_file)
        self.endpoint = lab.endpoints[endpoint]
        self.worker = threading.Thread(target=self._run, daemon=True)
        self.worker.start()

    def _run(self):
        try:
            self.asyncio.run(self._serve())
        except BaseException as error:
            if not self.stopping.is_set():
                self.errors.append(error)
        finally:
            self.listener.close()

    async def _serve(self):
        self.loop = self.asyncio.get_running_loop()
        self.shutdown = self.asyncio.Event()

        def accepted(reader, writer):
            task = self.asyncio.create_task(self._accept(reader, writer))
            self.tasks.add(task)
            task.add_done_callback(finished)

        def finished(task):
            self.tasks.discard(task)
            if not task.cancelled():
                error = task.exception()
                if error is not None and not self.stopping.is_set():
                    self.errors.append(error)

        server = await self.asyncio.start_server(
            accepted, sock=self.listener, ssl=self.server_tls, limit=16384,
            ssl_handshake_timeout=5, ssl_shutdown_timeout=1)
        try:
            if self.stopping.is_set():
                self.shutdown.set()
            await self.shutdown.wait()
        finally:
            server.close()
            pending = list(self.tasks)
            for task in pending:
                task.cancel()
            await self.asyncio.gather(*pending, return_exceptions=True)
            await server.wait_closed()

    async def _accept(self, reader, writer):
        writers = [writer]
        relays = []
        try:
            if self.stopping.is_set():
                return
            upstream_reader, upstream_writer = await self.asyncio.wait_for(
                self.asyncio.open_connection(
                    self.endpoint['host'], self.endpoint['port'], ssl=self.client_tls,
                    server_hostname=self.endpoint['host'], limit=16384,
                    ssl_handshake_timeout=5, ssl_shutdown_timeout=1), timeout=5)
            writers.append(upstream_writer)
            relays = [
                self.asyncio.create_task(self._relay(reader, upstream_writer, False)),
                self.asyncio.create_task(self._relay(upstream_reader, writer, True)),
            ]
            done, _ = await self.asyncio.wait(relays, return_when=self.asyncio.FIRST_COMPLETED)
            await self.asyncio.gather(*done)
        finally:
            for task in relays:
                task.cancel()
            await self.asyncio.gather(*relays, return_exceptions=True)
            for connection in writers:
                connection.close()
            await self.asyncio.gather(
                *(connection.wait_closed() for connection in writers), return_exceptions=True)

    async def _relay(self, source, destination, server):
        while not self.stopping.is_set():
            frame = await source.readline()
            if not frame:
                return
            if len(frame) > 16384 or not frame.endswith(b'\n'):
                raise AssertionError('Private history relay received an invalid IRC frame')
            line = frame.decode('utf-8', errors='replace').rstrip('\r\n')
            tags = {}
            if line.startswith('@'):
                tag_text, line = line.split(' ', 1)
                tags = dict(tag.partition('=')[::2] for tag in tag_text[1:].split(';'))
            if line.startswith(':'):
                line = line.split(' ', 1)[1]
            parts = line.split()
            if not server and len(parts) >= 3 and parts[0].upper() == 'CHATHISTORY':
                if parts[2].casefold() == self.target:
                    # Never retain authentication or unrelated client frames.
                    self.requests.append(line)
            if server and len(parts) >= 4 and parts[0].upper() == 'BATCH':
                if (parts[1].startswith('+') and parts[2].casefold() == 'chathistory'
                        and parts[3].lstrip(':').casefold() == self.target):
                    assert self.requests, 'History batch preceded the actual GUI request'
                    assert self.batch is None, 'Unexpected second target history batch'
                    self.batch = parts[1][1:]
            if (server and self.batch is not None and tags.get('batch') == self.batch
                    and not self.held.is_set()):
                self.held.set()
                # ponytail: pause this direction, preserving nested wire-frame ordering.
                if not await self.asyncio.to_thread(self.release.wait, 20):
                    raise TimeoutError('Private history body was not released before its deadline')
            destination.write(frame)
            await destination.drain()
            if server and self.batch is not None and parts[:2] == ['BATCH', '-' + self.batch]:
                self.completed.set()

    def ready(self, event):
        if self.errors:
            raise self.errors[0]
        return event.is_set()

    def close(self):
        self.stopping.set()
        self.release.set()
        if self.loop is not None and self.shutdown is not None:
            try:
                self.loop.call_soon_threadsafe(self.shutdown.set)
            except RuntimeError:
                pass  # The owning loop already closed after a reported relay error.
        self.worker.join(timeout=6)
        assert not self.worker.is_alive(), 'Private history TLS relay did not stop'


class _SojuSearchRelay(_StartupHistoryRelay):
    """Pass real Soju traffic unchanged, recording only search/history requests."""

    def __init__(self, lab, target):
        super().__init__(lab, target, 'soju')

    async def _relay(self, source, destination, server):
        while not self.stopping.is_set():
            frame = await source.readline()
            if not frame:
                return
            if len(frame) > 16384 or not frame.endswith(b'\n'):
                raise AssertionError('Invalid private Soju IRC frame')
            if not server:
                line = frame.decode('utf-8', errors='replace').rstrip('\r\n')
                command = line.split(' ', 1)[1] if line.startswith('@') else line
                if command.startswith(('SEARCH ', 'CHATHISTORY ')):
                    self.requests.append(command)
            destination.write(frame)
            await destination.drain()


def run_soju_search_context(ctx):
    """Click a remote-only Soju search hit and inspect its real surrounding history."""
    import os
    import sqlite3

    lab, observer = ctx['lab'], ctx['observer']
    endpoint = lab.endpoints['soju']
    channel = endpoint['channels'][0]
    identity = 'Soju/' + channel
    nonce = os.urandom(5).hex()
    marker = 'CONTEXTNEEDLE' + nonce
    colors = [(45, 155, 217), (42, 185, 113), (179, 79, 213)]
    bodies = [
        '\x042D9BD9,2D9BD9BEFORECONTEXT\x0f ' + nonce,
        '\x042AB971,2AB971' + marker + '\x0f original message',
        '\x04B34FD5,B34FD5AFTERCONTEXT\x0f ' + nonce,
    ]
    newer = ['Newer independent history ' + nonce + ' ' + str(i) for i in range(205)]
    destination = Path(ctx['destination'])
    config = Path(ctx['preferences']).with_name('config.toml')
    relay = _SojuSearchRelay(lab, channel)
    ctx['cleanup'].callback(relay.close)

    # Populate the bouncer while this GUI has never connected to it. More than
    # the client's 200-message page separates the old hit from LATEST.
    guest = dict(lab.endpoints['peer'], nick='context' + nonce[:8])
    with gui.fixtures.BootstrapClient(guest, lab.ca_file) as sender:
        sender.send('JOIN ' + channel)
        sender.wait(lambda command, params, prefix:
                    command == 'JOIN' and params == [channel] and
                    prefix.split('!', 1)[0] == guest['nick'])
        for text in bodies + newer:
            sender.send(f'PRIVMSG {channel} :{text}')
            sender.wait(lambda command, params, prefix:
                        command == 'PRIVMSG' and params == [channel, text] and
                        prefix.split('!', 1)[0] == guest['nick'])
            if text in bodies:
                time.sleep(0.05)

    gui.save_state(ctx['proc'], ctx['preferences'], ctx['database_path'])
    out, err = gui.finish_gui(ctx['proc'], ctx['window'], ctx['screenshot'],
                              destination.with_name('soju-search-before-restart.bmp'), ctx['args'].size)
    ctx['logs'].append(('soju-search-before-restart', out, err))
    departed = {ctx['own_nick'], ctx['other_nick']}
    while departed:
        _, _, prefix = observer.wait(lambda command, params, prefix:
            command == 'QUIT' and prefix.split('!', 1)[0] in departed, timeout=15)
        departed.remove(prefix.split('!', 1)[0])
    q = json.dumps
    with config.open('a') as stream:
        stream.write(
            f'\n[[identity]]\nname = "soju-probe"\nnick = {q(endpoint["nick"])}\n'
            f'username = {q(endpoint["username"])}\nrealname = "GUI search probe"\n'
            f'password = {q(endpoint["password"])}\n'
            f'[[server]]\nname = "Soju"\naddress = {q(endpoint["host"])}\n'
            f'port = {relay.port}\ntls = {q(endpoint["tls"])}\nidentity = "soju-probe"\n'
            f'sasl = {q(endpoint["sasl"])}\npassword = {q(endpoint["server_password"])}\n'
            f'bouncer = {q(endpoint["bouncer"])}\nchannels = {q(endpoint["channels"])}\n')
    screenshot = Path(ctx['screenshot']).with_name('soju-search-restart.bmp')
    proc, window = gui.launch_gui(ctx['binary'], screenshot, ctx['env'], ctx['args'].size, ctx['logs'])
    ctx.update(proc=proc, window=window, screenshot=screenshot)

    def close_restarted_gui():
        if not proc.native_debug_stream.closed:
            gui.stop_group(proc)
            out, err = gui.collect_gui(proc)
            ctx['logs'].append(('soju-search-restart-cleanup', out, err))

    ctx['cleanup'].callback(close_restarted_gui)
    gui.wait_joins(observer, ctx['channel'], (ctx['own_nick'], ctx['other_nick']))

    def rows(sql, params=()):
        with sqlite3.connect(f'file:{ctx["database_path"]}?mode=ro', uri=True) as db:
            return db.execute(sql, params).fetchall()

    gui.wait_rows(proc, ctx['database_path'],
                  'SELECT text FROM messages WHERE buffer=? AND text=?',
                  (identity, newer[-1]), [(newer[-1],)])
    _policy_focus(ctx, identity)
    draft = 'Unsent Soju source draft ' + nonce
    gui.type_text(draft)
    gui._composer_native(window, draft, len(draft), len(draft))
    gui.save_state(proc, ctx['preferences'], ctx['database_path'])
    assert not rows('SELECT text FROM messages WHERE buffer=? AND text IN (?,?,?)',
                    (identity, *bodies)), 'Old search context was already local'
    assert rows('SELECT draft FROM buffers WHERE id=?', (identity,)) == [(draft,)]

    gui.framed_keys(window, 'ctrl+s')
    gui.type_text(marker)
    gui.framed_keys(window, 'Return')
    gui.SESSION.until('search result hides source composer', lambda: gui.native_input_state(window) is None)
    result_box = _policy_wait_box(ctx, colors[1])
    search_capture = str(_policy_capture(ctx, 'soju-search-remote-result'))
    assert not rows('SELECT text FROM messages WHERE buffer=? AND text IN (?,?,?)',
                    (identity, *bodies)), 'SEARCH inserted the original into source history'
    point = ((result_box[0] + result_box[2]) / 2, (result_box[1] + result_box[3]) / 2)
    click_offset = gui.GUI_DEBUG[window][1].stat().st_size
    gui.click(window, *point)
    gui.await_native_frame(window, click_offset,
        re.compile(r'wl_pointer[@#]\d+\.button\(\d+,\s*\d+,\s*272,\s*0\)'))
    # The after-marker exists only in the source, not in the search result.
    # Wait for the asynchronous jump to render before scrolling its viewport.
    _policy_wait_box(ctx, colors[2])
    _policy_wait_box(ctx, colors[1])
    # Source jumps align the hit to the viewport top; reveal its preceding row
    # with a real wheel event before requiring all three context markers.
    x, y, width, height = gui.read_layout(ctx['preferences'])['conversation']['rect']
    gui.move_pointer(window, x + width / 2, y + height / 2)
    scroll_offset = gui.GUI_DEBUG[window][1].stat().st_size
    gui.POINTER.scroll(-3)
    gui.await_native_frame(window, scroll_offset,
        re.compile(r'wl_pointer[@#]\d+\.axis\(\d+,\s*0,'))
    boxes = [_policy_wait_box(ctx, color) for color in colors]
    assert all(a[3] < b[1] for a, b in zip(boxes, boxes[1:])), boxes
    source = gui.save_state(proc, ctx['preferences'], ctx['database_path'])
    assert source[1] == (identity,), ('Result click selected wrong source', source)
    upload.focus_composer(window)
    gui._composer_native(window, draft, len(draft), len(draft))
    originals = rows('SELECT id,nick,text,time FROM messages WHERE buffer=? AND text IN (?,?,?) '
                     'ORDER BY sort_time,time,seq', (identity, *bodies))
    assert [row[2] for row in originals] == bodies and all(row[0] and row[3] for row in originals), originals
    assert all(row[1] == guest['nick'] for row in originals), originals
    assert rows('SELECT draft FROM buffers WHERE id=?', (identity,)) == [(draft,)]
    requests = list(relay.requests)
    assert any(line.startswith('SEARCH ') for line in requests), requests
    around = [line for line in requests if line.startswith('CHATHISTORY AROUND ' + channel + ' timestamp=')]
    assert around, ('No real timestamp-selected Soju AROUND request', requests)
    return _refresh_return(ctx, 'soju-search-context', {
        'source': identity, 'search_marker': marker, 'search_click': point,
        'remote_only_before_click': True, 'newer_count': len(newer),
        'requests': requests, 'originals': originals, 'source_boxes': boxes,
        'source_draft': draft, 'source_selected': list(source[1]),
        'captures': [search_capture, str(_policy_capture(ctx, 'soju-search-source-context'))]})


def run_startup_history_sync(ctx):
    """Actual cached native chat stays usable while a real Ergo batch is held open."""
    destination = Path(ctx['destination'])
    config = Path(ctx['preferences']).with_name('config.toml')
    _policy_focus(ctx, ctx['channel_id'])
    gui.expand_navigation(ctx['proc'], ctx['window'], ctx['preferences'], ctx['database_path'])
    cached = 'CACHEDHISTORY retained native startup witness'
    remote = 'REMOTEHISTORY actual offline Ergo startup witness'
    _policy_send(ctx, cached, target=ctx['channel'])
    gui.save_state(ctx['proc'], ctx['preferences'], ctx['database_path'])
    layout = gui.read_layout(ctx['preferences'])
    x, y, width, height = layout['conversation']['rect']
    header = (x, y, width, gui.pane_metrics(layout, ctx['args'].zoom)['header_minimum'])

    def words(label, region):
        return upload.visible_words(ctx['window'], destination, 'startup-history-' + label,
                                    region=region, psm=11)

    def tokens(items):
        return {item['text'].casefold().strip('.,:;') for item in items}

    baseline = words('idle-before', header)
    assert upload.label_rect(ctx['window'], destination, 'CACHEDHISTORY',
                             region=layout['conversation']['rect']), 'Cached wire witness is not painted'
    baseline_capture = str(_policy_capture(ctx, 'startup-history-idle-before'))
    out, err = gui.finish_gui(ctx['proc'], ctx['window'], ctx['screenshot'],
        destination.with_name('startup-history-fixture-stop.bmp'), ctx['args'].size)
    ctx['logs'].append(('startup-history-fixture-stop', out, err))
    departed = {ctx['own_nick'], ctx['other_nick']}
    while departed:
        _, _, prefix = ctx['observer'].wait(lambda command, params, prefix:
            command == 'QUIT' and prefix.split('!', 1)[0] in departed, timeout=15)
        departed.remove(prefix.split('!', 1)[0])
    _policy_send(ctx, remote, target=ctx['channel'], durable=False)
    barrier = 'startup-history-offline-stored'
    ctx['observer'].send('PING :' + barrier)
    ctx['observer'].wait(lambda command, params, prefix:
        command == 'PONG' and params[-1] == barrier, timeout=10)
    original_config = config.read_text()
    relay = _StartupHistoryRelay(ctx['lab'], ctx['channel'])
    ctx['cleanup'].callback(relay.close)
    endpoint = ctx['lab'].endpoints['direct']
    profile = tomllib.loads(original_config)
    local = [server for server in profile['server']
             if server['name'] == 'Local' and server['port'] == endpoint['port']]
    assert len(local) == 1, 'Private Local profile was not uniquely identified'
    local[0]['port'] = relay.port
    # Match gui_smoke's generated profile writer: JSON quotes TOML scalar/array values.
    changed = ''.join(f'{key} = {json.dumps(value)}\n' for key, value in profile.items()
                      if key not in ('identity', 'server'))
    for section in ('identity', 'server'):
        for table in profile[section]:
            changed += f'\n[[{section}]]\n'
            changed += ''.join(f'{key} = {json.dumps(value)}\n' for key, value in table.items())
    assert tomllib.loads(changed) == profile, 'Private profile rewrite changed unrelated settings'
    config.write_text(changed)
    try:
        screenshot = Path(ctx['screenshot']).with_name('startup-history-restart.bmp')
        proc, window = gui.launch_gui(ctx['binary'], screenshot, ctx['env'], ctx['args'].size, ctx['logs'])
        ctx.update(proc=proc, window=window, screenshot=screenshot)

        def close_restarted_gui():
            if not proc.native_debug_stream.closed:
                gui.stop_group(proc)
                out, err = gui.collect_gui(proc)
                ctx['logs'].append(('startup-history-restart-cleanup', out, err))

        ctx['cleanup'].callback(close_restarted_gui)
        gui.SESSION.until('actual target CHATHISTORY body held after BATCH begin',
                          lambda: relay.ready(relay.held))
        started = time.monotonic()
        # Mapping and wire receipt can precede the restarted surface's first paint.
        def cached_frame():
            relay.ready(relay.held)
            observed = words('pending-conversation', layout['conversation']['rect'])
            return observed if 'cachedhistory' in tokens(observed) else None

        pending_words = gui.SESSION.until('cached native chat painted during actual history hold',
                                         cached_frame)
        assert 'cachedhistory' in tokens(pending_words), 'Pending history replaced the cached transcript'
        assert 'remotehistory' not in tokens(pending_words), 'Held remote history body was painted early'
        with sqlite3.connect(f"file:{ctx['database_path']}?mode=ro", uri=True) as database:
            assert database.execute('SELECT 1 FROM messages WHERE buffer=? AND text=?',
                (ctx['channel_id'], remote)).fetchall() == [], 'Held history body reached Core early'
        pending_header = [item for item in pending_words if int(item['top']) < y + header[3]]
        indication = tokens(pending_header) - tokens(baseline)
        assert indication, 'Actual pending header added no visible history indication'
        pending_capture = str(destination.with_name(
            destination.stem + '-startup-history-pending-conversation-desktop.png'))
        held_seconds = time.monotonic() - started
    finally:
        relay.release.set()
        config.write_text(original_config)
    gui.SESSION.until('original history BATCH end forwarded', lambda: relay.ready(relay.completed))
    gui.wait_joins(ctx['observer'], ctx['channel'], (ctx['own_nick'], ctx['other_nick']))
    gui.wait_messages(ctx['proc'], ctx['database_path'], ctx['expected_messages'])

    def idle_header():
        result = words('idle-after', header)
        return result if not indication.intersection(tokens(result)) else None

    final_header = gui.SESSION.until('native history indication retired after real batch end', idle_header)
    assert upload.label_rect(ctx['window'], destination, 'REMOTEHISTORY',
                             region=layout['conversation']['rect']), 'Actual history body is not painted'
    final_capture = str(_policy_capture(ctx, 'startup-history-idle-after'))
    return _refresh_return(ctx, 'startup-history-sync', {
        'actual_requests': relay.requests, 'actual_held_batch': relay.batch,
        'held_observation_seconds': held_seconds,
        'cached_message': cached, 'remote_history_message': remote,
        'dynamic_pending_header_tokens': sorted(indication),
        'idle_header_tokens': sorted(tokens(final_header)), 'header_region': header,
        'captures': [baseline_capture, pending_capture, final_capture],
        'tls_upstream_verified_private_ca': True, 'unchanged_real_batch_end_released': True})


def run_conversation_history(ctx):
    """Native C-o/C-i history, independent drafts, Tab and private binding migration."""
    p, w = ctx['proc'], ctx['window']
    prefs, db = ctx['preferences'], ctx['database_path']
    channel, query = ctx['channel_id'], ctx['query_id']
    _policy_focus(ctx, channel)
    _policy_focus(ctx, query)
    _policy_focus(ctx, channel)
    phases = []
    drafts = {
        channel: ('channel café draft', len('channel café'.encode()), len('channel café'.encode())),
        query: ('private DM draft', len('private DM'), len('private DM')),
    }

    def selected(identity, label, expected=None):
        expected = drafts[identity] if expected is None else expected
        native = gui._composer_native(w, *expected)
        state = gui.save_state(p, prefs, db)
        assert state[1] == (identity,), (label, state)
        assert gui.read_layout(prefs)['active_buffer'] == identity, (label, identity)
        restored = gui._composer_native(w, *expected)
        phases.append({'phase': label, 'selected': identity, 'native': native,
                       'after_private_save': restored})
        return restored

    def write_draft(identity):
        text, cursor, anchor = drafts[identity]
        gui.type_text(text)
        gui.framed_keys(w, 'Home', *(['Right'] * len(text.encode()[:cursor].decode())))
        return gui._composer_native(w, text, cursor, anchor)

    write_draft(channel)
    gui.switch_buffer(ctx['query_target'])
    upload.focus_composer(w)
    gui._composer_native(w, '', 0, 0)
    write_draft(query)
    selected(query, 'normal-DM-visit')
    gui.framed_keys(w, 'ctrl+o')
    selected(channel, 'CtrlO-restores-channel-draft-and-caret')
    gui.framed_keys(w, 'ctrl+i')
    selected(query, 'CtrlI-restores-DM-draft-and-caret')
    gui.framed_keys(w, 'ctrl+i')
    selected(query, 'forward-boundary-keeps-DM')

    # The switch picker owns these keys, not the workspace history consumer.
    gui._open_native_palette('ctrl+x', 'b', window=w)
    picker = gui._composer_native(w, '', 0, 0)
    gui.framed_keys(w, 'ctrl+o', 'ctrl+i')
    gui._composer_native(w, '', 0, 0)
    gui.framed_keys(w, 'Escape', 'ctrl+g')
    selected(query, 'picker-owns-history-chords')

    method = ctx['input_method']
    assert method is not None, 'conversation-history requires the owned native input method'
    method.drain()
    assert method.state['active'], method.state
    debug = gui.GUI_DEBUG[w][1]

    def preedit(text):
        offset = debug.stat().st_size
        serial = method.transaction(text, len(text), len(text))
        event = re.compile(r'zwp_text_input_v3[@#]\d+\.preedit_string\("' + re.escape(text) + r'",')
        gui.SESSION.until('actual history preedit delivery', lambda:
                          event.search(debug.read_bytes()[offset:].decode(errors='replace')))
        gui.await_native_frame(w, offset, event)
        return serial

    serial = preedit('history-ime')
    ime_before = gui.native_input_state(w)
    for chord in ('ctrl+o', 'ctrl+i'):
        offset = debug.stat().st_size
        presses = gui.key(chord, trace=True)
        release = re.compile(rf'wl_keyboard[@#]\d+\.key\(\d+,\s*\d+,\s*{presses[-1]},\s*0\)')
        gui.SESSION.until('native history chord release during preedit', lambda:
                          release.search(debug.read_bytes()[offset:].decode(errors='replace')))
        assert gui.native_input_state(w) == ime_before, (chord, gui.native_input_state(w))
    preedit('')
    selected(query, 'IME-owns-history-chords')

    gui.framed_keys(w, 'ctrl+o')
    selected(channel, 'back-after-modal-and-IME')
    # A normal selection after Back abandons the old forward branch.
    gui.switch_buffer(ctx['query_target'])
    selected(query, 'normal-selection-after-back')
    gui.framed_keys(w, 'ctrl+i')
    selected(query, 'normal-selection-is-new-forward-boundary')
    gui.clear_draft()
    gui._composer_native(w, '', 0, 0)
    gui.framed_keys(w, 'ctrl+o')
    selected(channel, 'Back-still-reaches-channel')
    gui.clear_draft()
    gui._composer_native(w, '', 0, 0)

    # Explicit Tab opens and accepts a nick completion; C-i above navigated instead.
    prefix = ctx['query_target'][:-1]
    gui.type_text(prefix)
    gui._composer_native(w, prefix, len(prefix), len(prefix))
    gui.framed_keys(w, 'Tab')
    gui._composer_native(w, prefix, len(prefix), len(prefix))
    gui.framed_keys(w, 'Return')
    completed = ctx['query_target'] + ': '
    completion = selected(channel, 'plain-Tab-completes-not-history',
                          (completed, len(completed), len(completed)))
    gui.clear_draft()
    gui._composer_native(w, '', 0, 0)

    bindings = tomllib.loads(Path(prefs).read_text())['bindings']
    # Model an older private profile: custom prefix wins over the absent new default.
    bindings.pop('conversation-back', None)
    bindings.pop('conversation-forward', None)
    bindings['search'] = 'C-o p'
    _refresh_restart(ctx, 'bindings', {key: json.dumps(value) for key, value in bindings.items()})
    p, w = ctx['proc'], ctx['window']
    saved = gui.save_preferences(p, prefs, w)['bindings']
    assert saved['search'] == 'C-o p' and saved['conversation-back'] == '', saved
    assert saved['conversation-forward'] == 'C-i', saved
    migrated = dict(saved)
    upload.focus_composer(w)
    gui._composer_native(w, '', 0, 0)
    gui.switch_buffer(ctx['query_target'])
    upload.focus_composer(w)
    gui.framed_keys(w, 'ctrl+o', 'p')
    gui._composer_native(w, '', 0, 0)
    gui.framed_keys(w, 'Escape', 'ctrl+g')
    selected(query, 'custom-prefix-search-preserved', ('', 0, 0))

    saved.update({'conversation-back': 'C-x h', 'conversation-forward': ''})
    _refresh_restart(ctx, 'bindings', {key: json.dumps(value) for key, value in saved.items()})
    p, w = ctx['proc'], ctx['window']
    saved = gui.save_preferences(p, prefs, w)['bindings']
    assert saved['conversation-back'] == 'C-x h', saved
    assert saved['conversation-forward'] == '' and saved['search'] == 'C-o p', saved
    upload.focus_composer(w)
    gui.switch_buffer(ctx['query_target'])
    upload.focus_composer(w)
    gui.framed_keys(w, 'ctrl+x', 'h')
    selected(channel, 'custom-back-binding-after-restart', ('', 0, 0))
    gui.framed_keys(w, 'ctrl+i')
    selected(channel, 'explicit-disabled-forward-stays-disabled', ('', 0, 0))
    return _refresh_return(ctx, 'conversation-history', {
        'phases': phases, 'picker': picker, 'ime_serial': serial, 'ime_native': ime_before,
        'plain_Tab_completion': completion, 'migrated_bindings': migrated,
        'custom_and_disabled_bindings': saved,
        'selection_evidence': 'native UTF-8 text/caret plus private saved active_buffer and layout',
    })


if __name__ == "__main__":
    main()
