#!/usr/bin/env python3
"""Real portal/HTTPS/soju consumer smoke; run inside nix develop, never on the host bus."""
import argparse
import base64
from contextlib import ExitStack
from email.parser import BytesParser
from email.policy import default
import http.server
import json
import os
from pathlib import Path
import queue
import re
import sqlite3
import shutil
import ssl
import subprocess
import tempfile
import threading
import tomllib

import gui_smoke as gui


class UploadServer:
    """A real TLS origin, not a proxy; each response is explicitly released by the scenario."""
    def __init__(self, lab):
        self.requests = queue.Queue()
        self.responses = queue.Queue()
        fixture = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass  # Never persist Basic credentials or request contents.

            def do_POST(self):
                length = int(self.headers.get("Content-Length", "0"))
                if not 0 < length <= 2 * 1048576:
                    self.send_error(413)
                    return
                body = self.rfile.read(length)
                fixture.requests.put((self.path, dict(self.headers), body))
                try:
                    status, text = fixture.responses.get(timeout=30)
                    payload = text.encode()
                    self.send_response(status)
                    self.send_header("Content-Type", "text/plain")
                    self.send_header("Content-Length", str(len(payload)))
                    self.end_headers()
                    self.wfile.write(payload)
                except (queue.Empty, BrokenPipeError, ConnectionResetError, ssl.SSLError):
                    pass

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(lab.root / "server.pem", lab.root / "server.key")
        self.server.socket = context.wrap_socket(self.server.socket, server_side=True)
        self.endpoint = f"https://localhost:{self.server.server_port}/upload"
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *exc):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)

    def receive(self, file, username, password, expiry):
        path, headers, body = self.requests.get(timeout=20)
        assert path == "/upload", path
        assert headers.get("Authorization") == "Basic " + base64.b64encode(
            f"{username}:{password}".encode()).decode(), "incorrect upload authentication"
        message = BytesParser(policy=default).parsebytes(
            b"Content-Type: " + headers["Content-Type"].encode() + b"\r\nMIME-Version: 1.0\r\n\r\n" + body)
        assert message.is_multipart(), "actual request must be multipart"
        parts = {part.get_param("name", header="content-disposition"): part
                 for part in message.iter_parts()}
        assert parts["file"].get_payload(decode=True) == file.read_bytes(), "actual file bytes differ"
        assert parts["file"].get_filename() == file.name
        assert parts["file"].get_content_type() == "text/plain", "native picker MIME did not reach the upload"
        for name, value in {"expires": expiry, "expiration": "never", "burn_after": "0",
                            "privacy": "unlisted", "syntax_highlight": "none",
                            "uploader_password": password}.items():
            assert parts[name].get_payload(decode=True).decode() == value, name
        assert "secret" in parts
        return {"body_bytes": len(body), "file_bytes": file.stat().st_size,
                "multipart_fields": sorted(parts), "basic_auth_verified": True}


class DesktopServices:
    """Own the bus and genuine freedesktop + GTK portal processes in the private compositor."""
    def __init__(self, env, output, portal, backend):
        self.env, self.output = env, output
        self.portal, self.backend = portal, backend
        self.children, self.streams = [], []

    def spawn(self, name, argv):
        stream = (self.output / (name + ".log")).open("wb")
        self.streams.append(stream)
        child = subprocess.Popen(argv, env=self.env, stdout=stream, stderr=subprocess.STDOUT,
                                 start_new_session=True)
        self.children.append(child)
        return child

    def __enter__(self):
        # HeadlessSway intentionally strips the host bus. Replace only its private environment.
        bus_config = self.output / "private-bus.conf"
        bus_config.write_text(
            "<busconfig><type>session</type><listen>unix:tmpdir=" +
            self.env["XDG_RUNTIME_DIR"] +
            "</listen><auth>EXTERNAL</auth><policy context=\"default\">"
            "<allow own=\"*\"/><allow send_destination=\"*\"/>"
            "<allow receive_sender=\"*\"/></policy></busconfig>")
        # No service directories: the missing-service case cannot silently autoactivate a host portal.
        bus = subprocess.Popen(["dbus-daemon", "--config-file", str(bus_config), "--nofork", "--print-address=1"],
                               env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               text=True, start_new_session=True)
        self.children.append(bus)
        address = bus.stdout.readline().strip()
        assert address.startswith("unix:"), "private D-Bus did not provide an address"
        self.env["DBUS_SESSION_BUS_ADDRESS"] = address
        self.env.update(XDG_CURRENT_DESKTOP="sway", GTK_USE_PORTAL="0", GDK_BACKEND="wayland")
        config = Path(self.env["XDG_CONFIG_HOME"]) / "xdg-desktop-portal"
        config.mkdir(parents=True, exist_ok=True)
        (config / "portals.conf").write_text("[preferred]\ndefault=gtk\norg.freedesktop.impl.portal.FileChooser=gtk\n")
        return self

    def start_portals(self):
        self.backend_process = self.spawn("portal-gtk", [str(self.backend)])
        self.spawn("portal", [str(self.portal)])
        for name in ("org.freedesktop.impl.portal.desktop.gtk", "org.freedesktop.portal.Desktop"):
            def owned():
                assert all(child.poll() is None for child in self.children), "private portal process exited"
                result = subprocess.run(["gdbus", "call", "--session", "--dest", "org.freedesktop.DBus",
                                         "--object-path", "/org/freedesktop/DBus", "--method",
                                         "org.freedesktop.DBus.NameHasOwner", name],
                                        env=self.env, capture_output=True, text=True, timeout=5)
                return result.returncode == 0 and "true" in result.stdout
            gui.SESSION.until("real portal owns " + name, owned)

    def __exit__(self, *exc):
        for child in reversed(self.children):
            gui.stop_group(child)
        for stream in self.streams:
            stream.close()


def tabs(window, count):
    gui.framed_keys(window, *(["Tab"] * count))


def saved_profiles(path):
    return tomllib.loads(path.read_text()).get("server", [])


def snapshot(proc, window, preferences, database):
    return gui.save_state(proc, preferences, database)


def paperclip(window, preferences, zoom):
    caret = gui.native_input_rectangle(window)
    assert caret is not None, "native text-input caret unavailable"
    # Native Input trailing accessory; geometry supplied by the production control owner.
    layout = gui.read_layout(preferences)
    primary = gui.composer_primary_targets(layout, zoom, caret)
    extent = primary["extent"]
    return primary["plus"][0] - extent / 2 - 4 * zoom - max(32, 32 * zoom) / 2, primary["plus"][1]


def control_popup_bounds(window, popup, point, zoom):
    """Check an actual source-control dropdown, not a pointer-context-menu placement policy."""
    trace = gui.GUI_DEBUG[window][1].read_text(errors="replace")
    requests = list(re.finditer(
        rf"xdg_surface[@#]{popup['xdg']}\.get_popup\(new id xdg_popup[@#]{popup['popup']},"
        r"\s*xdg_surface[@#](\d+),\s*xdg_positioner[@#](\d+)\)", trace))
    assert requests, "host popup lacks a real parent/positioner association"
    request = requests[-1]
    _, root_xdg, _ = gui.main_surface_ids(gui.GUI_DEBUG[window][1])
    assert int(request[1]) == root_xdg, "host popup is not owned by the invoking application root"
    anchors = list(re.finditer(
        rf"xdg_positioner[@#]{request[2]}\.set_anchor_rect\((-?\d+),\s*(-?\d+),\s*(\d+),\s*(\d+)\)",
        trace[:request.start()]))
    assert anchors, "host popup lacks its native source-control rectangle"
    ax, ay, aw, ah = map(int, anchors[-1].groups())
    target = max(32, 32 * zoom)
    assert ax <= point[0] < ax + aw and ay <= point[1] < ay + ah, (point, (ax, ay, aw, ah))
    assert abs(aw - target) <= 1 and abs(ah - target) <= 1, "host popup anchor is not the paperclip control"
    root = gui.window_geometry(window)
    assert 0 <= ax < ax + aw <= root["WIDTH"] and 0 <= ay < ay + ah <= root["HEIGHT"]
    sx, sy, sw, sh = popup["global_rect"]
    output = popup["output_rect"]
    assert output["x"] <= sx and output["y"] <= sy
    assert sx + sw <= output["x"] + output["width"] and sy + sh <= output["y"] + output["height"]
    gx, gy = root["X"] + ax, root["Y"] + ay
    gap = max(gx - (sx + sw), sx - (gx + aw), gy - (sy + sh), sy - (gy + ah), 0)
    assert gap <= target + 1, "dropdown surface is not adjacent to its source control"
    print("Native control-anchored host popup:", json.dumps({
        "anchor": (ax, ay, aw, ah), "invocation": point, "surface": popup["global_rect"],
        "source_root": root_xdg, "surface_to_control_gap": gap}), flush=True)


def choose_host(window, preferences, zoom, destination, index=6):
    point = paperclip(window, preferences, zoom)
    before = gui.window_geometry(window)
    popup = gui.open_native_popup(window, lambda: gui.click(window, *point))
    gui.capture_native_popup(window, popup, destination, "source-hosts")
    control_popup_bounds(window, popup, point, zoom)
    assert gui.window_geometry(window) == before, "host dropdown changed the source window geometry"
    gui.popup_keys(window, popup, "Home", *(["Down"] * index), "Return")


def visible_words(window, destination, label="locator", *, region=None, psm=None, min_confidence=30):
    """Read genuine rendered pixels for control locators and scoped caption assertions."""
    import csv
    import io
    from PIL import Image
    image = destination.with_name(destination.stem + "-" + label + ".png")
    desktop = destination.with_name(destination.stem + "-" + label + "-desktop.png")
    geometry = gui.window_geometry(window)
    screen_width, screen_height = gui.display_geometry()
    # Park the real cursor inside the window's blank edge, not over a button label.
    gui.move_pointer(window, max(8, 8 - geometry["X"]),
                     min(geometry["HEIGHT"], screen_height - geometry["Y"]) - 8)
    gui.capture_png(gui.SESSION.env, desktop, scale=1,
                    expected_pixels=(screen_width, screen_height), output="HEADLESS-1")
    if gui.window_geometry(window) != geometry:
        return []  # No safe label coordinates while the genuine window allocation is changing.
    rx, ry, rw, rh = region if region is not None else (0, 0, geometry["WIDTH"], geometry["HEIGHT"])
    left, top = max(0, round(geometry["X"] + rx)), max(0, round(geometry["Y"] + ry))
    right = min(screen_width, round(geometry["X"] + rx + rw))
    bottom = min(screen_height, round(geometry["Y"] + ry + rh))
    if left >= right or top >= bottom:
        return []
    # Read actual visible pixels even while a newly mapped GTK chooser exceeds
    # the minimum-size output, then translate label coordinates back to its owned root.
    recognition_scale = 3 if region is not None else 1
    with Image.open(desktop) as captured:
        cropped = captured.crop((left, top, right, bottom))
        if recognition_scale != 1:
            cropped = cropped.resize((cropped.width * recognition_scale, cropped.height * recognition_scale),
                                     Image.Resampling.LANCZOS)
        cropped.save(image)
    mode = psm if psm is not None else (6 if region is not None else 11)
    result = subprocess.run(["tesseract", str(image), "stdout", "--psm", str(mode), "tsv"],
                            check=True, capture_output=True, text=True, timeout=15)
    words = [row for row in csv.DictReader(io.StringIO(result.stdout), delimiter="\t", quoting=csv.QUOTE_NONE)
             if row["text"].strip() and float(row["conf"]) >= min_confidence]
    for row in words:
        row["left"] = str(int(row["left"]) // recognition_scale + left - geometry["X"])
        row["top"] = str(int(row["top"]) // recognition_scale + top - geometry["Y"])
        for dimension in ("width", "height"):
            row[dimension] = str((int(row[dimension]) + recognition_scale - 1) // recognition_scale)
    return words


def label_rect(window, destination, text, *, region=None, multiline=False, exact_caption=False):
    wanted = text.casefold().split()
    words = visible_words(window, destination, region=region)
    if region is not None:
        x, y, width, height = region
        words = [word for word in words if x <= int(word["left"]) < x + width
                 and y <= int(word["top"]) < y + height]
    matches = []
    for index in range(len(words) - len(wanted) + 1):
        found = words[index:index + len(wanted)]
        if [word["text"].casefold() for word in found] != wanted:
            continue
        if not multiline and max(int(word["top"]) for word in found) - min(int(word["top"]) for word in found) > 12:
            continue
        left = min(int(word["left"]) for word in found)
        top = min(int(word["top"]) for word in found)
        right = max(int(word["left"]) + int(word["width"]) for word in found)
        bottom = max(int(word["top"]) + int(word["height"]) for word in found)
        if exact_caption:
            line = tuple(found[0][key] for key in ("block_num", "par_num", "line_num"))
            if any(tuple(word[key] for key in ("block_num", "par_num", "line_num")) != line
                   for word in found):
                continue
            gap = max(int(word["height"]) for word in found) * 3
            caption = sorted((word for word in words
                if tuple(word[key] for key in ("block_num", "par_num", "line_num")) == line
                and int(word["left"]) + int(word["width"]) >= left - gap
                and int(word["left"]) <= right + gap), key=lambda word: int(word["left"]))
            if " ".join(word["text"].strip() for word in caption).casefold().split() != wanted:
                continue
        matches.append((left, top, right - left, bottom - top))
    return max(matches, key=lambda rect: rect[1]) if matches else None


def click_label(window, destination, text, *, region=None):
    rect = gui.SESSION.until("rendered native control " + text,
                             lambda: label_rect(window, destination, text, region=region))
    x, y, width, height = rect
    marker = gui.GUI_DEBUG[window][1].stat().st_size
    gui.click(window, x + width / 2, y + height / 2)
    gui.await_native_frame(window, marker, re.compile(r"wl_pointer[@#]\d+\.button\("))


def upload_panel_region(window):
    preferences = Path(gui.SESSION.env["XDG_CONFIG_HOME"]) / "blastirc/settings.toml"
    # Locator only: image and retained-text reviews may grow with their content.
    # Observe real filename/size controls and native preview pixels separately.
    return gui.read_layout(preferences)["conversation"]["rect"]


def panel_action(window, destination, text):
    click_label(window, destination, text, region=upload_panel_region(window))


def native_choose(window, file, destination, services):
    def chooser():
        candidates = [node for node, _ in gui.tree_windows(gui.SESSION.ipc("get_tree"))
                      if node.get("pid") == services.backend_process.pid and
                      node.get("shell") == "xdg_shell" and node.get("name") == "Choose a file"]
        assert len(candidates) <= 1, "multiple owned portal chooser roots"
        return candidates[0] if candidates else None
    dialog = gui.SESSION.until("genuine owned GTK portal file chooser", chooser)
    identifier = dialog["id"]
    print("Owned native portal chooser:", json.dumps({
        key: dialog.get(key) for key in ("id", "pid", "app_id", "name", "shell")}), flush=True)
    try:
        # A mapped, focused portal-owned root accepts native input. GTK header wording
        # and its compositor-managed window size are not file-selection invariants.
        gui.SESSION.ipc("command", f"[con_id={identifier}] focus")
        gui.SESSION.until("owned GTK chooser receives keyboard input", lambda:
                          bool(gui.native_window(identifier).get("focused")))
        gui.SESSION.ipc("command", f"[con_id={identifier}] floating enable")
        display_width, display_height = gui.display_geometry()
        gui.SESSION.ipc("command",
                        f"[con_id={identifier}] resize set {min(1100, display_width)} {min(800, display_height)}, "
                        "move position 0 0, focus")
        def visible_allocation():
            geometry = gui.window_geometry(identifier)
            return (geometry["WIDTH"] > 0 and geometry["HEIGHT"] > 0 and
                    geometry["X"] < display_width and geometry["Y"] < display_height and
                    geometry["X"] + geometry["WIDTH"] > 0 and geometry["Y"] + geometry["HEIGHT"] > 0 and
                    gui.native_window(identifier).get("focused"))
        gui.SESSION.until("owned focused chooser intersects the private output", visible_allocation)
        # The requested 1100x800 is only a locator convenience, never a client-size acceptance requirement.
        ready = gui.native_rgb_capture(identifier, geometry={
            "X": 0, "Y": 0, "WIDTH": display_width, "HEIGHT": display_height}, capture_timing=True)
        assert len(ready["capture_timestamps"]) == 1, "chooser capture lacks a real ready timestamp"
        destination.with_suffix(".chooser-capture.json").write_text(json.dumps({
            "root": identifier, "chooser_geometry": gui.window_geometry(identifier),
            "geometry": ready["geometry"], "capture": ready["capture_timestamps"],
        }, indent=2) + "\n")
        gui.capture_png(gui.SESSION.env, destination.with_name(destination.stem + "-native-chooser.png"),
                        scale=1, expected_pixels=gui.display_geometry(), output="HEADLESS-1")
        # Use a visible Recent row, otherwise the owned file's full native location.
        row = label_rect(identifier, destination, file.name)
        if row is None:
            gui.key("ctrl+l", "ctrl+a")
            gui.type_text(str(file))
            gui.capture_png(gui.SESSION.env, destination.with_name(destination.stem + "-native-file-location.png"),
                            scale=1, expected_pixels=gui.display_geometry(), output="HEADLESS-1")
        else:
            gui.click(identifier, row[0] + row[2] / 2, row[1] + row[3] / 2)
        gui.capture_png(gui.SESSION.env, destination.with_name(destination.stem + "-native-selected-file.png"),
                        scale=1, expected_pixels=gui.display_geometry(), output="HEADLESS-1")
        gui.SESSION.until("selected-file chooser retains native keyboard focus", lambda:
                          bool(gui.native_window(identifier).get("focused")))
        geometry = gui.window_geometry(identifier)
        # The genuine GTK header-bar accept Button is at the root's upper-right,
        # 6px from the edge; choose an interior point independent of header OCR.
        accept = (geometry["WIDTH"] - 48, 23)
        output_width, output_height = gui.display_geometry()
        if (0 <= geometry["X"] + accept[0] < output_width and
                0 <= geometry["Y"] + accept[1] < output_height):
            gui.click(identifier, *accept)
        else:
            # GTK may grow its location-entry window after mapping on a small desktop.
            # The selected row or full location names our file; use the same native consent,
            # never a pointer outside the real output or an injected portal response.
            gui.key("Return")
        gui.SESSION.until("the selected portal chooser root unmaps after consent", lambda:
                          all(node["id"] != identifier for node, _ in gui.tree_windows(gui.SESSION.ipc("get_tree"))))
    except BaseException:
        gui.capture_png(gui.SESSION.env, destination.with_name(destination.stem + "-native-chooser-failed.png"),
                        scale=1, expected_pixels=gui.display_geometry(), output="HEADLESS-1")
        raise
    gui.SESSION.ipc("command", f"[con_id={window}] focus")
    gui.SESSION.until("source application allocation restored after native chooser", lambda:
                      (gui.window_geometry(window)["WIDTH"], gui.window_geometry(window)["HEIGHT"]) == gui.display_geometry())
    selected_size = file.stat().st_size
    gui.SESSION.until("real portal selection reaches source-labelled filename and size review", lambda:
                      label_rect(window, destination, file.name, region=upload_panel_region(window), multiline=True) and
                      label_rect(window, destination, f"{selected_size} bytes",
                                 region=upload_panel_region(window), multiline=True))
    gui.capture_window(window, destination, "public-source-review")


def set_upload_settings(proc, window, preferences, database, destination, endpoint):
    # This helper is shared by fixtures with different initial upload settings.
    # Read the live application's native save, rather than blindly inverting its toggle.
    initial = gui.save_preferences(proc, preferences, window)["uploads"]
    gui.open_settings(window, preferences)
    # open_settings focuses the first content control, not category navigation.
    gui.framed_keys(window, "shift+Tab")
    gui.settings_category(window, 1, 2)
    title = gui.SESSION.until("actual Chat upload-provider field rendered", lambda:
                             label_rect(window, destination, "Default file host"))
    x, y, width, height = title
    # The native Select is immediately below its rendered label (4px group gap, 32px control).
    popup = gui.open_native_popup(window, lambda: gui.click(window, x + 30, y + height + 22))
    gui.capture_native_popup(window, popup, destination, "settings-upload-provider")
    gui.popup_keys(window, popup, "Home", *(["Down"] * 6), "Return", root_changed=False)
    gui.framed_keys(window, "Tab", "shift+Tab")
    # Rebuild uses the existing page policy: first content control is the provider.
    tabs(window, 1)
    gui.settings_field(window, "2", destination, "upload-size")
    tabs(window, 1)
    if not initial["secret"]:
        gui.framed_keys(window, "space")
    tabs(window, 1)
    gui.settings_field(window, "12", destination, "upload-expiry")
    tabs(window, 1)
    gui.settings_field(window, endpoint, destination, "upload-endpoint")
    tabs(window, 1)
    gui.settings_field(window, "gui-upload", destination, "upload-authcid")
    tabs(window, 1)
    gui.framed_keys(window, "ctrl+a", "BackSpace")
    gui.type_text("private-upload-password")
    # Return to category without relying on a fixed number of unrelated later Chat controls.
    gui.framed_keys(window, *(["shift+Tab"] * 7))
    saved = gui.save_settings_footer(window, saved_path=preferences, destination=destination)
    expected = {"provider": "custom", "custom_endpoint": endpoint, "username": "gui-upload",
                "password": "private-upload-password", "expiry_hours": "12", "size_limit_mib": 2, "secret": True}
    assert all(saved["uploads"].get(key) == value for key, value in expected.items()), "Settings roundtrip differs"
    gui.close_settings_navigation(window)
    return expected


class FilehostObserver(gui.WireObserver):
    """Record genuine registration/ISUPPORT evidence without retaining credential-bearing frames."""
    def __init__(self, *args, control=False, **kwargs):
        self.control = control
        super().__init__(*args, **kwargs)

    def send(self, line):
        if self.control and line.startswith("CAP REQ :"):
            offered = {cap.split("=", 1)[0] for frame in self.frames
                       if frame["command"] == "CAP" and "LS" in frame["params"]
                       for cap in frame["params"][-1].split()}
            if "soju.im/bouncer-networks" in offered:
                line += " soju.im/bouncer-networks"
        super().send(line)

    def advertisement_evidence(self, registration_ended):
        from urllib.parse import urlsplit
        advertised = self.isupport.get("soju.im/FILEHOST", "")
        endpoint = urlsplit(advertised)
        registration = [frame["command"] for frame in self.frames if frame["command"] in ("903", "001")]
        return {"filehost_advertised": bool(advertised), "scheme": endpoint.scheme,
                "hostname": endpoint.hostname, "port": endpoint.port, "path": endpoint.path,
                "authenticated_before_welcome": ("903" in registration and "001" in registration and
                                                  registration.index("903") < registration.index("001")),
                "bouncer_networks_negotiated": "soju.im/bouncer-networks" in self.capabilities,
                "registration_end_received": registration_ended}


class GuiTopology(gui.fixtures.LocalTopology):
    """Use soju's own HTTPS file service and two genuinely connected upstream networks."""
    def _spawn(self, name, args, environment=None):
        if name == "soju":
            self.file_port = self._port("filehost")
            self._release("filehost")
            config = self.root / "soju.conf"
            with config.open("a") as stream:
                stream.write(f"listen https://127.0.0.1:{self.file_port}\n"
                             f"http-ingress https://{self.endpoints['soju']['host']}:{self.file_port}\n"
                             f"file-upload fs {self.root}/uploads\n")
        return super()._spawn(name, args, environment)

    def _soju(self, port, upstream):
        super()._soju(port, upstream)
        second = self._endpoint(upstream["port"], "sojuother", "sojuother")
        self._register(second)
        control = ["sojuctl", "-config", str(self.root / "soju.conf"), "user", "run", "fixture"]
        self._run(*control, "network", "create", "-name", "other",
                  "-addr", f'ircs://127.0.0.1:{second["port"]}',
                  "-nick", second["nick"], "-username", second["username"], "-enabled=false")
        self._run(*control, "sasl", "set-plain", "-network", "other", second["username"], second["password"])
        self._run(*control, "channel", "create", "#blastirc-soju-other/other")
        self._run(*control, "network", "update", "other", "-enabled=true")
        endpoint = dict(self.endpoints["soju"], username="fixture/other")
        with gui.fixtures.BootstrapClient(endpoint, self.ca_file) as downstream:
            self._verify_upstream(downstream, second["nick"], "#blastirc-soju-other")
        self.filehost_evidence = {}
        for role, username in (("bound", "fixture/local"), ("control", "fixture")):
            with FilehostObserver(dict(self.endpoints["soju"], username=username),
                                  self.ca_file, control=role == "control") as downstream:
                ended = True
                try:
                    downstream.wait(lambda command, params, prefix: command in ("376", "422"), timeout=5)
                except TimeoutError:
                    ended = False
                self.filehost_evidence[role] = downstream.advertisement_evidence(ended)


def buffer_rows(database):
    with sqlite3.connect(f"file:{database}?mode=ro", uri=True) as connection:
        return connection.execute("SELECT id,unread,draft FROM buffers ORDER BY id").fetchall()


def assert_no_chat_url(database, url):
    with sqlite3.connect(f"file:{database}?mode=ro", uri=True) as connection:
        assert connection.execute("SELECT COUNT(*) FROM messages WHERE text LIKE ?", ("%" + url + "%",)).fetchone() == (0,), \
            "upload must not send a chat message"


def focus_composer(window):
    preferences = Path(gui.SESSION.env["XDG_CONFIG_HOME"]) / "blastirc/settings.toml"
    layout = gui.read_layout(preferences)
    zoom = layout["scale"]
    metrics = gui.pane_metrics(layout, zoom)
    x, y, width, height = layout["conversation"]["rect"]
    state = gui.native_input_state(window)
    if state is not None and state["caret"] is not None:
        cx, cy, cw, ch = state["caret"]
        point = (cx + cw / 2, cy + ch / 2)
    else:
        # Empty/non-text focus has no current caret. Locate the native primary Input
        # in its current allocated pane, to the left of the nonmodal upload panel.
        point = (x + metrics["left"] + 16 * zoom, y + height - metrics["padding_y"] - 16 * zoom)
    gui.click(window, *point)
    gui.framed_keys(window, "ctrl+End")
    def focused():
        current = gui.native_input_state(window)
        return (current is not None and current["caret"] is not None and
                current["caret"][1] + current["caret"][3] >= point[1] - 64 * zoom)
    # Refocusing an already active Input need not emit another text-input enable.
    gui.SESSION.until("native composer caret after actual pointer activation and Ctrl+End", focused)


def probe_accessory(proc, window, preferences, database, destination, ime, zoom=1):
    """Raw empty, control-only and real preedit, with native hit/tab consumers."""
    focus_composer(window)
    gui._composer_native(window, "", 0, 0)
    point = paperclip(window, preferences, zoom)
    gui.capture_window(window, destination, "empty-dim-paperclip")
    popup = gui.open_native_popup(window, lambda: gui.key("shift+Tab", "shift+Tab", "Return"))
    gui.capture_native_popup(window, popup, destination, "keyboard-host-icons")
    gui.popup_keys(window, popup, "Escape")
    for text, label in (("typed", "typed"), ("\x02", "format-only")):
        # Escape returns to the invoking paperclip Button, not necessarily to its Input.
        focus_composer(window)
        gui._composer_native(window, "", 0, 0)
        if text == "\x02":
            gui.format_native_control(window, 0, destination, "format-only")
        else:
            gui.type_text(text)
        length = len(text.encode("utf-8"))
        actual = gui._composer_native(window, text, length, length)
        assert actual["text"] == text, actual
        marker = gui.GUI_DEBUG[window][1].stat().st_size
        gui.click(window, *point)
        gui.framed_keys(window, "shift+Tab", "shift+Tab")
        assert gui.native_input_state(window) is not None, "hidden accessory retained a tab stop before Plus"
        trace = gui.GUI_DEBUG[window][1].read_bytes()[marker:].decode(errors="replace")
        assert ".get_popup(" not in trace, "hidden upload accessory remained a native hit/tab target"
        gui.capture_window(window, destination, "hidden-" + label)
        gui.key("Escape")
        focus_composer(window)
        gui.clear_draft()
        gui._composer_native(window, "", 0, 0)
    ime.drain()
    marker = gui.GUI_DEBUG[window][1].stat().st_size
    ime.transaction("猫", 0, 3)
    gui.SESSION.until("actual preedit commit reaches native client", lambda:
                      ".preedit_string(" in gui.GUI_DEBUG[window][1].read_bytes()[marker:].decode(errors="replace"))
    gui.click(window, *point)
    gui.move_pointer(window, point[0] - 1, point[1])
    paint = gui.GUI_DEBUG[window][1].stat().st_size
    gui.move_pointer(window, point[0], point[1])
    gui.await_native_frame(window, paint, re.compile(r"wl_pointer[@#]\d+\.motion\("))
    trace = gui.GUI_DEBUG[window][1].read_bytes()[marker:].decode(errors="replace")
    assert ".get_popup(" not in trace, "preedit upload accessory remained hit/tab reachable"
    gui.capture_window(window, destination, "hidden-preedit")
    ime.drain()
    assert ime.state["active"], "hidden accessory click displaced the actual composing Input"
    ime.transaction("", 0, 0)
    gui._composer_native(window, "", 0, 0)


def probe_uploads(proc, window, preferences, database, destination, services, fixture, file, ime, second_child):
    settings = set_upload_settings(proc, window, preferences, database, destination, fixture.endpoint)
    focus_composer(window)
    probe_accessory(proc, window, preferences, database, destination, ime)
    # No portal is running yet: the genuine missing-service error must be presented, not swallowed.
    choose_host(window, preferences, 1, destination)
    gui.SESSION.until("visible missing-portal failure", lambda:
                      label_rect(window, destination, "Close", region=upload_panel_region(window)))
    gui.capture_window(window, destination, "missing-portal-error")
    panel_action(window, destination, "Close")
    services.start_portals()
    evidence = []
    for mode in ("success", "failure-retry", "cancel", "edit", "switch", "preedit"):
        label = destination.with_name(destination.stem + "-" + mode)
        focus_composer(window)
        gui.clear_draft()
        origin = snapshot(proc, window, preferences, database)[1][0]
        choose_host(window, preferences, 1, label)
        native_choose(window, file, label, services)
        assert fixture.requests.empty(), "file chooser/review must not start an upload"
        panel_action(window, label, "Upload")
        received = fixture.receive(file, settings["username"], settings["password"], settings["expiry_hours"])
        gui.SESSION.until("visible genuine upload progress actions", lambda:
                          label_rect(window, label, "Cancel", region=upload_panel_region(window)))
        gui.capture_window(window, label, "progress")
        url = fixture.endpoint.replace("/upload", "/file/" + mode)
        if mode == "failure-retry":
            fixture.responses.put((503, "service unavailable"))
            gui.SESSION.until("visible explicit retry", lambda:
                              label_rect(window, label, "Retry upload", region=upload_panel_region(window)))
            assert fixture.requests.empty(), "upload failure retried automatically"
            gui.capture_window(window, label, "failure")
            panel_action(window, label, "Retry upload")
            fixture.receive(file, settings["username"], settings["password"], settings["expiry_hours"])
        if mode == "cancel":
            panel_action(window, label, "Cancel")
            fixture.responses.put((201, url))
            gui.SESSION.until("visible upload cancellation", lambda:
                              label_rect(window, label, "Close", region=upload_panel_region(window)))
            gui.capture_window(window, label, "cancelled")
            panel_action(window, label, "Close")
            focus_composer(window)
            gui._composer_native(window, "", 0, 0)
        else:
            if mode == "edit":
                focus_composer(window)
                gui.type_text("newer draft")
            elif mode == "switch":
                gui.switch_buffer("#blastirc-soju-other", second_child)
                gui._composer_native(window, "second independent draft", 24, 24)
            elif mode == "preedit":
                focus_composer(window)
                ime.transaction("猫", 0, 3)
                gui.SESSION.until("preedit is active during held HTTPS response", lambda:
                                  gui.native_input_state(window) is not None and
                                  gui.native_input_state(window)["text"] == "")
                held_preedit = gui.native_input_rectangle(window)
            fixture.responses.put((201, url))
            if mode in ("edit", "preedit"):
                gui.SESSION.until("completed URL retained for original source", lambda:
                                  label_rect(window, label, "Copy", region=upload_panel_region(window)))
                gui.capture_window(window, label, "source-recovery")
                if mode == "preedit":
                    ime.drain()
                    assert ime.state["active"] and gui.native_input_rectangle(window) == held_preedit, \
                        "completed upload displaced active native composition"
                    gui._composer_native(window, "", 0, 0)
                else:
                    gui._composer_native(window, "newer draft", 11, 11)
                panel_action(window, label, "Copy")
                gui.SESSION.until("completed URL on real clipboard", lambda: gui.clipboard_text() == url)
                panel_action(window, label, "Close")
                focus_composer(window)
                if mode == "preedit":
                    ime.transaction("", 0, 0)
                    gui._composer_native(window, "", 0, 0)
                else:
                    gui._composer_native(window, "newer draft", 11, 11)
            else:
                if mode == "switch":
                    focus_composer(window)
                    gui._composer_native(window, "second independent draft", 24, 24)
                    gui.switch_buffer(origin.split("/", 1)[1], origin.split("/", 1)[0])
                focus_composer(window)
                gui._composer_native(window, url, len(url.encode()), len(url.encode()))
                gui.command("undo", window=window)
                gui._composer_native(window, "", 0, 0)
                gui.framed_keys(window, "ctrl+shift+z")
                gui._composer_native(window, url, len(url.encode()), len(url.encode()))
                gui.clear_draft()
        snapshot(proc, window, preferences, database)
        assert_no_chat_url(database, url)
        assert tomllib.loads(preferences.read_text())["uploads"]["provider"] == "custom", \
            "per-upload host selection changed the persisted default"
        evidence.append({"scenario": mode, "source": origin, "request": received, "no_autosend": True})
    return evidence


def probe_origin_lifecycle(proc, window, preferences, database, destination, fixture, file,
                           observer, first_child, lab, services):
    current_nick = "guiobserver"
    evidence = []
    for mode in ("rename", "delete"):
        label = destination.with_name(destination.stem + "-" + mode)
        observer.send("PRIVMSG soju :origin identity " + mode)
        original = first_child + "/" + current_nick
        gui.wait_rows(proc, database, "SELECT text FROM messages WHERE buffer=? AND text=?",
                      (original, "origin identity " + mode), [("origin identity " + mode,)])
        gui.switch_buffer(current_nick, first_child)
        focus_composer(window)
        gui.clear_draft()
        choose_host(window, preferences, 1, label)
        native_choose(window, file, label, services)
        panel_action(window, label, "Upload")
        fixture.receive(file, "gui-upload", "private-upload-password", "12")
        if mode == "rename":
            previous = current_nick
            current_nick = "guirenamed"
            observer.send("NICK " + current_nick)
            gui.receive_until(observer, "NICK", [current_nick], previous, redact=lab._redact)
            gui.SESSION.until("real IRC NICK remaps source query", lambda:
                              any(row[0] == first_child + "/" + current_nick for row in buffer_rows(database)))
        else:
            gui.command("close-buffer", window=window)
            gui.SESSION.until("original query really removed", lambda:
                              all(row[0] != original for row in buffer_rows(database)))
        # Persist the actual renamed/fallback pane before locating its conversation-bounded recovery panel.
        snapshot(proc, window, preferences, database)
        url = fixture.endpoint.replace("/upload", "/file/" + mode)
        fixture.responses.put((201, url))
        if mode == "rename":
            # A fresh-target NICK rename preserves the actual native document identity.
            # It is not a merge/deletion: the unchanged draft receives one undoable insertion.
            gui.switch_buffer(current_nick, first_child)
            focus_composer(window)
            length = len(url.encode("utf-8"))
            gui._composer_native(window, url, length, length)
            gui.capture_window(window, label, "renamed-document-url-inserted")
            gui.command("undo", window=window)
            gui._composer_native(window, "", 0, 0)
            gui.framed_keys(window, "ctrl+shift+z")
            gui._composer_native(window, url, length, length)
            gui.clear_draft()
            gui._composer_native(window, "", 0, 0)
            snapshot(proc, window, preferences, database)
            assert_no_chat_url(database, url)
            evidence.append({"scenario": mode, "original_document": original,
                             "renamed_document": first_child + "/" + current_nick,
                             "URL_inserted_with_one_undo_redo": True, "no_autosend": True})
            continue
        gui.SESSION.until("deleted source retains completed link", lambda:
                          label_rect(window, label, "Copy", region=upload_panel_region(window)))
        gui.capture_window(window, label, "source-url-recovery")
        panel_action(window, label, "Insert")
        gui.SESSION.until("failed deleted-source insertion retains recovery", lambda:
                          label_rect(window, label, "Copy", region=upload_panel_region(window)))
        panel_action(window, label, "Copy")
        gui.SESSION.until("source recovery copies exact server URL", lambda: gui.clipboard_text() == url)
        panel_action(window, label, "Close")
        snapshot(proc, window, preferences, database)
        assert_no_chat_url(database, url)
        evidence.append({"scenario": mode, "original_document": original, "completed_URL_recovered": True})
    return evidence


def rail_point(preferences, index, *, grouped=True):
    x, y, width, _ = gui.read_layout(preferences)["rail"]["rect"]
    inner = width - 12
    return x + width / 2, y + 6 + (3 if grouped else 0) + (inner + 12) / 2 + index * (inner + 18)


def rail_menu(window, preferences, index, destination, choice, *, grouped=True):
    point = rail_point(preferences, index, grouped=grouped)
    def right_click():
        gui.move_pointer(window, *point)
        gui.POINTER.button(True, 0x111)
        gui.POINTER.button(False, 0x111)
    popup = gui.open_native_popup(window, right_click)
    gui.capture_native_popup(window, popup, destination, "source-server-menu")
    gui.select_native_menu(window, popup, [choice])


def assert_parent_alias_caption(window, preferences, destination, label):
    x, y, width, _ = gui.read_layout(preferences)["rail"]["rect"]
    inner = width - 12
    # Scale-1 grouped parent: caption sits below the avatar, inside the first rail item.
    region = (x + 8, y + inner + 1, width - 16, 20)
    gui.SESSION.until("actual parent rail caption Home", lambda:
                      any(word["text"] == "Home" for word in
                          visible_words(window, destination, label, region=region)))
    gui.capture_window(window, destination, label)


def settings_close_point(window):
    """Locate the last sticky footer Button inside the native scale-1 Settings allocation."""
    bounds = gui.window_geometry(window)
    width, height = min(1000, bounds["WIDTH"] - 24), min(720, bounds["HEIGHT"] - 24)
    # Dialog chrome has 12px padding; choose an interior point in its rightmost footer Button.
    return (bounds["WIDTH"] + width) / 2 - 32, (bounds["HEIGHT"] + height) / 2 - 30


def scroll_settings(window, destination, steps):
    """Fence delivery even at a scroll limit, then use actual screencopy timestamps to settle."""
    bounds = gui.window_geometry(window)
    close = settings_close_point(window)
    # Focused native single-line Inputs consume wheel events as caret movement.
    # Wheel the actual trailing content gutter, outside Inputs and before the scrollbar.
    gui.move_pointer(window, close[0], bounds["HEIGHT"] * .55)
    path = gui.GUI_DEBUG[window][1]
    marker = path.stat().st_size
    gui.POINTER.scroll(steps)
    gui.SESSION.until("actual native wheel delivery", lambda:
                      re.search(r"wl_pointer[@#]\d+\.axis", path.read_bytes()[marker:].decode(errors="replace")))
    # A clamped wheel need not repaint. Hover a real Button without moving focus or scrolling it into view.
    marker = path.stat().st_size
    gui.move_pointer(window, *close)
    gui.await_native_frame(window, marker, re.compile(r"wl_pointer[@#]\d+\.motion\("))
    previous = started = None
    def settled():
        nonlocal previous, started
        image = gui.native_rgb_capture(window, capture_timing=True)
        stamps = image["capture_timestamps"]
        assert len(stamps) == 1, "native capture did not report its real ready timestamp"
        stamp = stamps[0]["timestamp_ns"]
        with destination.with_suffix(".capture-times.jsonl").open("a") as stream:
            stream.write(json.dumps({"purpose": "native-settings-scroll", **stamps[0]}) + "\n")
        pixels = bytearray(image["pixels"])
        caret = gui.native_input_rectangle(window)
        if caret:
            x, y, width, height = caret
            for row in range(max(0, y - 2), min(image["height"], y + height + 2)):
                start = (row * image["width"] + max(0, x - 2)) * 3
                end = (row * image["width"] + min(image["width"], x + width + 2)) * 3
                pixels[start:end] = bytes(end - start)
        if pixels != previous:
            previous, started = pixels, stamp
            return False
        return stamp - started >= 50_000_000  # Actual compositor timestamps, never subprocess duration.
    gui.SESSION.until("stable native settings pixels across genuine capture timestamps", settled)


def seek_label(window, destination, text, *, control_below=0):
    """Scroll the actual settings viewport until a rendered locator is in view."""
    bounds = gui.window_geometry(window)
    height = min(720, bounds["HEIGHT"] - 24)
    # Scale-1 modal: 12px chrome padding, 38px sticky footer, 8px body/footer gap.
    viewport_bottom = (bounds["HEIGHT"] + height) / 2 - 58
    scroll_settings(window, destination, -1000)
    for _ in range(45):
        rect = label_rect(window, destination, text)
        if rect is not None and rect[1] + rect[3] + control_below < viewport_bottom:
            return rect
        scroll_settings(window, destination, 8)
    raise AssertionError("native settings control is not reachable: " + text)


def remote_field(window, destination, name, value):
    x, y, width, height = seek_label(window, destination, "Remote " + name, control_below=44)
    gui.click(window, x + min(width / 2, 40), y + height + 22)
    gui.framed_keys(window, "ctrl+a", "BackSpace")
    gui.type_text(value)
    gui.framed_keys(window, "End")
    if name not in ("pass", "sasl-password"):
        length = len(value.encode())
        gui._composer_native(window, value, length, length)


def remote_catalog(lab):
    with sqlite3.connect(f"file:{lab.root / 'soju.db'}?mode=ro", uri=True) as connection:
        connection.row_factory = sqlite3.Row
        return [dict(row) for row in connection.execute("SELECT * FROM Network")]


def close_native_settings(window, destination):
    marker = gui.GUI_DEBUG[window][1].stat().st_size
    gui.click(window, *settings_close_point(window))
    gui.await_native_frame(window, marker,
                           re.compile(r"wl_pointer[@#]\d+\.button\(\d+,\s*\d+,\s*272,\s*0\)"))
    gui.capture_window(window, destination, "native-settings-closed")
    gui.SETTINGS_OPEN[window] = False


def discover_children(proc, window, preferences, config, database):
    def children():
        gui.save_preferences(proc, preferences, window)
        return [profile for profile in saved_profiles(config) if profile.get("soju_parent") == "Account"]
    discovered = gui.SESSION.until("two real discovered soju child profiles", lambda:
                                   (rows if len(rows := children()) == 2 else None))
    channels = {}
    for profile in discovered:
        # IDs are opaque Core keys. Determine routing from real restored channel buffers, never captions.
        def channel():
            return next((row[0] for row in buffer_rows(database)
                         if row[0].startswith(profile["name"] + "/#")), None)
        channels[profile["name"]] = gui.SESSION.until("separate child channel attachment", channel)
    assert len(set(channels.values())) == 2
    first = next(key for key, value in channels.items() if value.endswith("/#blastirc-soju"))
    second = next(key for key in channels if key != first)
    return first, second, discovered, channels


def configure_soju_parent(proc, window, preferences, config, destination, lab):
    """Authenticate the existing private parent through its real one-page native form."""
    initial = tomllib.loads(config.read_text())
    parent = next(profile for profile in initial["server"] if profile["name"] == "Account")
    identity = next(row for row in initial["identity"] if row["name"] == parent["identity"])
    assert parent["sasl"] == "" and not parent["soju_enabled"]
    assert identity["password"] == "" and len(initial["server"]) == 2
    gui.open_settings(window, preferences)
    gui.framed_keys(window, "shift+Tab")  # First content control -> native category navigation.
    gui.settings_category(window, 1, 5)  # Networks; existing Account is the initial profile.
    x, y, width, height = seek_label(window, destination, "Authentication method")
    popup = gui.open_native_popup(window, lambda: gui.click(window, x + 30, y + height + 22))
    gui.capture_native_popup(window, popup, destination, "parent-authentication-choices")
    gui.popup_keys(window, popup, "Home", "Return")  # Password (SASL PLAIN).

    def account_field(label, value, *, masked=False):
        x, y, width, height = seek_label(window, destination, label, control_below=44)
        point = (x + min(width / 2, 40), y + height + 22)
        marker = gui.GUI_DEBUG[window][1].stat().st_size
        gui.click(window, *point)
        def focused():
            state = gui.native_input_state(window)
            trace = gui.GUI_DEBUG[window][1].read_bytes()[marker:].decode(errors="replace")
            if state is None or state["caret"] is None:
                return False
            cx, cy, cw, ch = state["caret"]
            return (abs(cy + ch / 2 - point[1]) < 24 and cx >= x and
                    re.search(r"zwp_text_input_v3[@#]\d+\.enable\(\)", trace) and
                    (not masked or re.search(r"\.set_content_type\(\d+,\s*8\)", trace)))
        gui.SESSION.until("fresh native " + label + (" masked" if masked else "") + " Input focus", focused)
        gui.framed_keys(window, "ctrl+a", "BackSpace")
        gui.type_text(value)
        gui.framed_keys(window, "End")
        if not masked:
            length = len(value.encode())
            gui._composer_native(window, value, length, length)
        gui.capture_window(window, destination, "parent-" + label.lower().replace(" ", "-"))

    username = lab.endpoints["soju"]["username"].split("/", 1)[0]
    account_field("Account username", username)
    account_field("Account password", lab.endpoints["soju"]["password"], masked=True)
    tabs(window, 4)  # Edit identity -> empty Auto-join -> Bouncer -> Connect automatically Toggle.
    gui.framed_keys(window, "space")
    tabs(window, 2)  # Empty Trusted soju file-host Input -> actual Save and connect Button.
    before = config.stat()
    gui.framed_keys(window, "Return")
    def saved():
        if proc.poll() is not None:
            raise AssertionError("GUI exited while saving parent credentials")
        try:
            current = config.stat()
            if (current.st_ino, current.st_mtime_ns) == (before.st_ino, before.st_mtime_ns):
                return False
            actual = tomllib.loads(config.read_text())
        except (FileNotFoundError, tomllib.TOMLDecodeError):
            return False
        profile = next(row for row in actual["server"] if row["name"] == "Account")
        account = next(row for row in actual["identity"] if row["name"] == identity["name"])
        return actual if (profile["sasl"] == "PLAIN" and profile["soju_enabled"] and
                          account["username"] == username and
                          account["password"] == lab.endpoints["soju"]["password"]) else False
    actual = gui.SESSION.until("fresh native Save and connect writes PLAIN and shared Identity credentials", saved)
    profile = next(row for row in actual["server"] if row["name"] == "Account")
    assert profile["identity"] == identity["name"] and len(actual["identity"]) == len(initial["identity"])
    assert not profile.get("password"), "account password was duplicated into Server PASS"
    scroll_settings(window, destination, -1000)
    gui.capture_window(window, destination, "parent-native-connection-status")
    seek_label(window, destination, "Remote catalog received.")
    gui.capture_window(window, destination, "parent-native-authenticated-catalog")
    close_native_settings(window, destination)
    return {"initial_none_empty_password_startup_disabled": True,
            "native_password_auth_shared_identity_saved": True,
            "masked_password_native_input": True, "registered_parent_catalog_received": True}


def probe_network_names(proc, window, preferences, config, database, destination, lab):
    setup = configure_soju_parent(proc, window, preferences, config, destination, lab)
    first, second, discovered, channels = discover_children(proc, window, preferences, config, database)
    identities = {row["name"]: (row["soju_parent"], row["soju_network_id"]) for row in discovered}
    catalog = {str(row["id"]): row["name"] for row in remote_catalog(lab)}
    assert catalog[identities[first][1]] == "local"
    assert catalog[identities[second][1]] == "other"
    original_buffers = {row[0] for row in buffer_rows(database)}
    gui.close_settings_navigation(window)
    # Start elsewhere so activating the result proves routing, not just an unchanged selection.
    gui.switch_buffer("#blastirc-soju-other", catalog[identities[second][1]])
    selected = gui.save_state(proc, preferences, database)
    assert selected[1] == (channels[second],), "human caption selected the wrong starting child"
    gui.close_settings_navigation(window)
    geometry = gui.window_geometry(window)
    width, height = min(620, geometry["WIDTH"] - 20), min(480, geometry["HEIGHT"] - 30)
    left, top = (geometry["WIDTH"] - width) / 2, (geometry["HEIGHT"] - height) / 2
    result_region = (left + 16, top + 104, width - 32, height - 120)
    captions = {}

    def caption(expected, label, absent=()):
        def rendered():
            words = visible_words(window, destination, label, region=result_region)
            tokens = [word["text"].strip("()[],:;|").casefold() for word in words]
            text = " ".join(tokens)
            if expected.casefold() not in tokens or any(value.casefold() in text for value in absent):
                return False
            return text
        captions[label] = gui.SESSION.until("actual palette network caption " + expected, rendered)
        gui.capture_window(window, destination, label)

    gui._open_native_palette("ctrl+x", "b", window=window)
    gui.type_text("#blastirc-soju-other")
    gui._palette_query(window, "#blastirc-soju-other")
    caption("other", "network-names-other", (second,))
    gui.framed_keys(window, "ctrl+a", "BackSpace")
    gui.type_text("#blastirc-soju")
    gui._palette_query(window, "#blastirc-soju")
    # This prefix also matches the second channel; activate the actual renamed row below.
    caption("local", "network-names-before", (first,))
    netid = identities[first][1]
    with FilehostObserver(dict(lab.endpoints["soju"], username="fixture"),
                          lab.ca_file, control=True) as control:
        assert "soju.im/bouncer-networks" in control.capabilities
        control.send(f"BOUNCER CHANGENETWORK {netid} name=renamed")
        command, params, _ = control.wait(
            lambda command, params, prefix:
                (command == "BOUNCER" and params[:2] == ["CHANGENETWORK", netid]) or
                (command == "FAIL" and params[:1] == ["BOUNCER"]))
        assert command == "BOUNCER", ("catalog rename rejected", params)
        gui.SESSION.until("acknowledged rename in real soju catalog", lambda:
                          any(str(row["id"]) == netid and row["name"] == "renamed"
                              for row in remote_catalog(lab)))
        # No GUI input or Settings snapshot between these captures: the palette stays open.
        caption("renamed", "network-names-live", ("local", first))
    click_label(window, destination, "renamed", region=result_region)
    state = gui.save_state(proc, preferences, database)
    assert state[1] == (channels[first],), "renamed palette result changed its routing target"
    after = {row["name"]: (row.get("soju_parent"), row.get("soju_network_id"))
             for row in saved_profiles(config) if row.get("soju_parent") == "Account"}
    assert after == identities, "catalog rename changed stable child keys or parent links"
    assert {row[0] for row in buffer_rows(database)} == original_buffers, "catalog rename changed buffer IDs"
    gui.close_settings_navigation(window)
    return {"native_setup": setup, "rendered_captions": captions,
            "acknowledged_rename": {"network_id": netid, "before": "local", "after": "renamed"},
            "child_identity": identities, "child_buffers": channels,
            "activated_original_buffer": channels[first],
            "palette_remained_open_during_rename": True,
            "stable_child_and_buffer_ids_preserved": True}


def probe_soju(proc, window, preferences, config, database, destination, lab, observer):
    def children():
        gui.save_preferences(proc, preferences, window)
        return [profile for profile in saved_profiles(config) if profile.get("soju_parent") == "Account"]
    setup = configure_soju_parent(proc, window, preferences, config, destination, lab)
    first, second, discovered, channels = discover_children(proc, window, preferences, config, database)
    gui.switch_buffer("#blastirc-soju", first)
    gui.type_text("first independent draft")
    snapshot(proc, window, preferences, database)
    gui.switch_buffer("#blastirc-soju-other", second)
    gui.type_text("second independent draft")
    snapshot(proc, window, preferences, database)
    observer.send("PRIVMSG #blastirc-soju :independent unread proof")
    gui.wait_rows(proc, database, "SELECT text FROM messages WHERE buffer=? AND text=?",
                  (channels[first], "independent unread proof"), [("independent unread proof",)])
    # Message insertion and buffer metadata are separate durable writes; normal autosave is 30s.
    # Flush via the real native Settings Save after observing this exact incoming body's destination.
    selected = snapshot(proc, window, preferences, database)
    assert selected[1] == (channels[second],), "incoming sibling traffic changed the selected child"
    gui.wait_rows(proc, database, "SELECT unread>0,draft FROM buffers WHERE id=?",
                  (channels[first],), [(1, "first independent draft")])
    rows = {row[0]: row[1:] for row in buffer_rows(database)}
    assert rows[channels[first]][0] > 0 and rows[channels[first]][1] == "first independent draft", rows
    assert rows[channels[second]][1] == "second independent draft", rows
    before = snapshot(proc, window, preferences, database)
    gui.capture_window(window, destination, "expanded-capsule-connected-glyph-titlebar")
    gui.click(window, *rail_point(preferences, 0))
    after = snapshot(proc, window, preferences, database)
    assert after[1] == before[1] and after[2] == before[2], "parent fold changed conversation state"
    # save_state intentionally projects workspace navigation fields, excluding Soju fold intent.
    gui.wait_preference(proc, preferences, "workspace", "soju-group:Account", "collapsed")
    assert tomllib.loads(preferences.read_text())["workspace"]["soju-group:Account"] == "collapsed"
    gui.capture_window(window, destination, "collapsed-active-unread-capsule")
    gui.click(window, *rail_point(preferences, 0))
    snapshot(proc, window, preferences, database)
    gui.wait_preference(proc, preferences, "workspace", "soju-group:Account", "expanded")
    assert tomllib.loads(preferences.read_text())["workspace"]["soju-group:Account"] == "expanded"
    # Edit the parent display alias through its real context action; stable routing keys must not move.
    profiles_before_alias = saved_profiles(config)
    parent_before_alias = next(profile for profile in profiles_before_alias if profile["name"] == "Account")
    child_links = {profile["name"]: profile["soju_parent"] for profile in profiles_before_alias
                   if profile.get("soju_parent")}
    fold_before_alias = {key: value for key, value in tomllib.loads(preferences.read_text())["workspace"].items()
                         if key.startswith("soju-group:")}
    rail_menu(window, preferences, 0, destination, 2)  # Open server log, Connection, Edit profile.
    gui.SETTINGS_OPEN[window] = True
    tabs(window, 3)  # Network selector -> Add network -> Add soju account -> Account alias.
    gui.settings_field(window, "Home", destination, "parent-account-alias-input")
    gui.framed_keys(window, *(["shift+Tab"] * 4))  # Alias -> three profile controls -> category navigation.
    saved_alias = gui.save_settings_footer(window, saved_path=config, destination=destination,
                                           label="parent-account-alias-saved")
    assert next(profile for profile in saved_alias["server"] if profile["name"] == "Account") == \
        {**parent_before_alias, "alias": "Home"}, "alias edit changed the stable parent profile"
    assert {profile["name"]: profile["soju_parent"] for profile in saved_alias["server"]
            if profile.get("soju_parent")} == child_links, "alias edit changed child routing links"
    assert {key: value for key, value in tomllib.loads(preferences.read_text())["workspace"].items()
            if key.startswith("soju-group:")} == fold_before_alias, "alias edit changed stable fold keys"
    gui.close_settings_navigation(window)
    assert_parent_alias_caption(window, preferences, destination, "parent-account-alias-rail")
    # Local disable is independent of the actual upstream connection and retains local documents.
    second_index = 1 + next(index for index, profile in enumerate(discovered) if profile["name"] == second)
    rail_menu(window, preferences, second_index, destination, 2)
    gui.SETTINGS_OPEN[window] = True
    tabs(window, 3)  # profile picker, Add network, Add soju account -> local attachment switch
    gui.framed_keys(window, "space")
    gui.SESSION.until("local child disabled in saved profile", lambda:
                      any(profile["name"] == second and not profile.get("soju_enabled", True)
                          for profile in saved_profiles(config)))
    remote_id = next(profile["soju_network_id"] for profile in discovered if profile["name"] == second)
    assert any(str(row["id"]) == remote_id and row["enabled"] for row in remote_catalog(lab)), \
        "local disable changed the actual remote upstream network"
    assert any(row[0] == channels[second] and row[2] == "second independent draft"
               for row in buffer_rows(database))
    gui.capture_window(window, destination, "local-disable-retains-remote-and-draft")
    gui.framed_keys(window, "space")
    gui.SESSION.until("local child reenabled", lambda:
                      any(profile["name"] == second and profile.get("soju_enabled", True)
                          for profile in saved_profiles(config)))
    close_native_settings(window, destination)
    # Saved but never attached standalone profile, after parent+children.
    rail_menu(window, preferences, 3, destination, 0)
    log = snapshot(proc, window, preferences, database)
    assert log[1] == ("Saved/*server*",), log
    gui.capture_window(window, destination, "saved-source-server-log-no-channel-row")
    gui.switch_buffer("#blastirc-soju", first)
    gui.clear_draft()
    # Native management must reach actual BOUNCER acknowledgements, not optimistic local state.
    baseline_ids = {str(row["id"]) for row in remote_catalog(lab)}
    rail_menu(window, preferences, 0, destination, 4)
    gui.SETTINGS_OPEN[window] = True
    remote_field(window, destination, "name", "gui-created")
    remote_field(window, destination, "host", "127.0.0.1")
    remote_field(window, destination, "port", str(lab.endpoints["direct"]["port"]))
    remote_field(window, destination, "nickname", "gui-created")
    # Continue from nickname through the six remaining Inputs and Remote TLS to the real submit Button.
    # Its same-named section heading is intentionally not used as a click target.
    tabs(window, 8)
    gui.framed_keys(window, "Return")
    created = gui.SESSION.until("real soju ADDNETWORK acknowledgement and DB insertion", lambda:
                               next((row for row in remote_catalog(lab) if row["name"] == "gui-created"), None))
    assert str(created["id"]) not in baseline_ids
    gui.capture_window(window, destination, "remote-add-ack")
    close_native_settings(window, destination)
    new_child = gui.SESSION.until("created child materialized from catalog", lambda:
                                next((profile for profile in children()
                                      if profile.get("soju_network_id") == str(created["id"])), None))
    profiles = children()
    child_index = 1 + next(index for index, profile in enumerate(profiles) if profile["name"] == new_child["name"])
    rail_menu(window, preferences, child_index, destination, 0)
    focus_composer(window)
    gui.type_text("/join #gui-retained")
    gui.framed_keys(window, "Return")
    # JOIN creates the local buffer in memory; metadata otherwise waits for the 30s autosave.
    snapshot(proc, window, preferences, database)
    retained_id = new_child["name"] + "/#gui-retained"
    gui.SESSION.until("new child actual joined conversation", lambda:
                      any(row[0] == retained_id for row in buffer_rows(database)))
    observer.send("JOIN #gui-retained")
    gui.receive_until(observer, "JOIN", ["#gui-retained"], "guiobserver", redact=lab._redact)
    observer.send("PRIVMSG #gui-retained :retained real remote history")
    gui.wait_rows(proc, database, "SELECT text FROM messages WHERE buffer=? AND text=?",
                  (retained_id, "retained real remote history"), [("retained real remote history",)])
    focus_composer(window)
    gui.type_text("retained local draft")
    snapshot(proc, window, preferences, database)
    rail_menu(window, preferences, child_index, destination, 3)
    gui.SETTINGS_OPEN[window] = True
    remote_field(window, destination, "name", "gui-renamed")
    tabs(window, 11)  # Nine remaining remote Inputs, TLS, then the genuine Apply Button.
    gui.framed_keys(window, "Return")
    gui.SESSION.until("real CHANGENETWORK acknowledgement", lambda:
                      any(str(row["id"]) == str(created["id"]) and row["name"] == "gui-renamed"
                          for row in remote_catalog(lab)))
    gui.capture_window(window, destination, "remote-change-ack")
    # A real server-side validation FAIL, followed by explicit correction, not local validation.
    remote_field(window, destination, "name", "local")  # Duplicate real remote name is rejected by soju.
    tabs(window, 11)
    gui.framed_keys(window, "Return")
    seek_label(window, destination, "Request failed:")
    gui.SESSION.until("native server rejection visible", lambda:
                      any(word["text"].casefold() in ("failed", "invalid", "failure", "rejected")
                          for word in visible_words(window, destination)))
    assert any(str(row["id"]) == str(created["id"]) and row["name"] == "gui-renamed"
               for row in remote_catalog(lab))
    gui.capture_window(window, destination, "remote-real-fail")
    remote_field(window, destination, "name", "gui-corrected")
    tabs(window, 11)
    gui.framed_keys(window, "Return")
    gui.SESSION.until("explicit correction acknowledged", lambda:
                      any(str(row["id"]) == str(created["id"]) and row["name"] == "gui-corrected"
                          for row in remote_catalog(lab)))
    close_native_settings(window, destination)
    # Delete confirmation is genuinely separate; local history is retained after the real acknowledgement.
    retained = buffer_rows(database)
    rail_menu(window, preferences, child_index, destination, 4)
    gui.SETTINGS_OPEN[window] = True
    gui.capture_window(window, destination, "remote-delete-confirm-local-retention")
    assert any(str(row["id"]) == str(created["id"]) for row in remote_catalog(lab))
    bounds = gui.window_geometry(window)
    zoom = tomllib.loads(preferences.read_text())["appearance"]["scale"]
    confirm_width = min(540 * zoom, bounds["WIDTH"] - 24)
    confirm_height = min(250 * zoom, bounds["HEIGHT"] - 24)
    footer = ((bounds["WIDTH"] - confirm_width) / 2,
              (bounds["HEIGHT"] + confirm_height) / 2 - 80 * zoom, confirm_width, 80 * zoom)
    # The confirmation has Cancel then Delete in this real footer; click inside
    # Delete rather than treating matching body prose or OCR spelling as consent.
    marker = gui.GUI_DEBUG[window][1].stat().st_size
    gui.click(window, footer[0] + confirm_width / 3, footer[1] + footer[3] / 2)
    gui.await_native_frame(window, marker, re.compile(r"wl_pointer[@#]\d+\.button\("))
    gui.SESSION.until("real DELNETWORK acknowledgement", lambda:
                      all(str(row["id"]) != str(created["id"]) for row in remote_catalog(lab)))
    close_native_settings(window, destination)
    assert {row[0] for row in retained} <= {row[0] for row in buffer_rows(database)}, \
        "remote deletion purged local history"
    gui.switch_buffer("#gui-retained", new_child["name"])
    focus_composer(window)
    gui._composer_native(window, "retained local draft", 20, 20)
    gui.wait_rows(proc, database, "SELECT text FROM messages WHERE buffer=? AND text=?",
                  (retained_id, "retained real remote history"), [("retained real remote history",)])
    gui.capture_window(window, destination, "deleted-remote-retained-history-and-draft")
    gui.switch_buffer("#blastirc-soju", first)
    return first, second, {**setup, "children": channels, "remote_add_change_delete_acknowledged": True,
                           "remote_FAIL_observed": True, "local_history_retained": True,
                           "parent_alias": "Home", "parent_alias_stable_routing_and_fold": True}


def probe_filehost(proc, window, preferences, database, destination, lab, file, services):
    import urllib.request
    from urllib.parse import urlsplit
    focus_composer(window)
    gui.clear_draft()
    choose_host(window, preferences, 1, destination, index=7)
    native_choose(window, file, destination, services)
    panel_action(window, destination, "Upload")
    focus_composer(window)
    def completed():
        state = gui.native_input_state(window)
        return state["text"] if state and state["text"].startswith("https://") else None
    url = gui.SESSION.until("actual Soju FILEHOST URL inserted", completed)
    parsed = urlsplit(url)
    assert parsed.hostname == lab.endpoints["soju"]["host"] and parsed.port == lab.file_port
    context = ssl.create_default_context(cafile=lab.ca_file)
    # Fetch from the genuine built-in HTTPS service, never an intercepted or rewritten endpoint.
    with urllib.request.urlopen(url, context=context, timeout=10) as response:
        assert response.read() == file.read_bytes(), "Soju hosted bytes differ from selected native file"
    gui.capture_window(window, destination, "genuine-soju-filehost-url")
    assert tomllib.loads(preferences.read_text())["uploads"]["provider"] == "custom"
    assert_no_chat_url(database, url)
    gui.command("undo", window=window)
    gui._composer_native(window, "", 0, 0)
    return {"genuine_soju_discovery_auth_and_https_service": True, "downloaded_bytes": file.stat().st_size,
            "per_upload_choice_preserved_default": True, "no_autosend": True}


def run():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", nargs="?", type=Path, default=gui.ROOT / "build-native/blastirc")
    parser.add_argument("--output-dir", type=Path, default=Path("/tmp/blastirc-upload-soju"))
    parser.add_argument("--scope", choices=("uploads", "soju", "network-names", "all"), default="all",
                        help="independent native consumer scope; default all preserves the complete workflow")
    parser.add_argument("--portal", type=Path, default=os.environ.get("BLASTIRC_PORTAL"))
    parser.add_argument("--portal-backend", type=Path, default=os.environ.get("BLASTIRC_PORTAL_BACKEND"))
    args = parser.parse_args()
    if args.portal is None or args.portal_backend is None:
        parser.error("use pinned nix develop BLASTIRC_PORTAL/BLASTIRC_PORTAL_BACKEND or explicit --portal/--portal-backend")
    binary = args.binary.resolve()
    for executable in ("sway", "swaymsg", "wtype", "wl-copy", "wl-paste", "grim", "pkg-config",
                       "dbus-daemon", "gdbus", "tesseract", str(binary), str(args.portal), str(args.portal_backend)):
        if shutil.which(executable) is None:
            parser.error(f"{executable!r} unavailable; invoke inside nix develop")
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True, mode=0o700)
    output.chmod(0o700)
    destination = output / "consumer.bmp"
    with GuiTopology() as lab, tempfile.TemporaryDirectory(prefix="blastirc-upload-soju-") as temporary, ExitStack() as cleanup:
        root = Path(temporary)
        for name in ("home", "config/blastirc", "data", "state", "cache"):
            (root / name).mkdir(parents=True, mode=0o700)
        config = root / "config/blastirc/config.toml"
        preferences = config.with_name("settings.toml")
        database = root / "data/blastirc/state.sqlite3"
        endpoint = lab.endpoints["soju"]
        q = json.dumps
        native_soju_setup = args.scope in ("soju", "network-names", "all")
        config.write_text(
            "schema_version = 1\n"
            '[[identity]]\nname = "soju-user"\nnick = "soju"\nusername = "fixture"\n'
            f'password = {q("" if native_soju_setup else endpoint["password"])}\nrealname = "Native upload proof"\n'
            '[[server]]\nname = "Account"\n'
            f'address = {q(endpoint["host"])}\nport = {endpoint["port"]}\n'
            f'tls = true\nidentity = "soju-user"\nsasl = {q("" if native_soju_setup else "PLAIN")}\n'
            'bouncer = "soju"\nsoju_account = true\n' +
            ('soju_enabled = false\n' if native_soju_setup else '') +
            '[[server]]\nname = "Saved"\naddress = "localhost"\nport = 9\ntls = true\n'
            'identity = "soju-user"\nsoju_enabled = false\n')
        config.chmod(0o600)
        preferences.write_text('[appearance]\nscale = 1.0\nreduced_motion = true\n[uploads]\nsecret = false\n')
        preferences.chmod(0o600)
        file = root / "private chosen attachment.txt"
        file.write_bytes(("Synthetic private native chooser payload 猫\r\n" * 1024).encode())
        file.chmod(0o600)
        env = dict(os.environ, HOME=str(root / "home"), XDG_CONFIG_HOME=str(root / "config"),
                   XDG_DATA_HOME=str(root / "data"), XDG_STATE_HOME=str(root / "state"),
                   XDG_CACHE_HOME=str(root / "cache"), SSL_CERT_FILE=lab.ca_file,
                   CURL_CA_BUNDLE=lab.ca_file, LIBGL_ALWAYS_SOFTWARE="1")
        for name in ("DBUS_STARTER_ADDRESS", "DBUS_STARTER_BUS_TYPE", "SSH_AUTH_SOCK"):
            env.pop(name, None)
        if os.environ.get("BLASTIRC_PORTAL_BACKEND_DIR"):
            env["XDG_DESKTOP_PORTAL_DIR"] = os.environ["BLASTIRC_PORTAL_BACKEND_DIR"]
        gui.SESSION = cleanup.enter_context(gui.HeadlessSway(
            root / "runtime", output / "compositor", env, outputs=(("HEADLESS-1", 1440, 940, 0, 0, 1),)))
        env = gui.SESSION.env
        gui.POINTER = cleanup.enter_context(gui.VirtualSeat(Path(env["XDG_RUNTIME_DIR"]) / env["WAYLAND_DISPLAY"]))
        gui.SESSION.until("owned native pointer and keyboard", lambda:
                          {"pointer", "keyboard"} <= {device.get("type") for device in gui.SESSION.ipc("get_inputs")})
        ime = cleanup.enter_context(gui.NativeInputMethod(gui.POINTER))
        services = cleanup.enter_context(DesktopServices(env, output, args.portal, args.portal_backend))
        fixture = cleanup.enter_context(UploadServer(lab))
        observer_endpoint = dict(lab.endpoints["direct"], nick="guiobserver", username="guiobserver")
        observer = cleanup.enter_context(gui.WireObserver(observer_endpoint, lab.ca_file, authenticate=False))
        for channel in ("#blastirc-soju", "#blastirc-soju-other"):
            observer.send("JOIN " + channel)
            gui.receive_until(observer, "JOIN", [channel], "guiobserver", redact=lab._redact)
        logs = []
        proc = window = None
        report = {"verification": "real native consumer flow", "fixture": "owned Ergo + soju built-in HTTPS",
                  "soju_wire_discovery": lab.filehost_evidence,
                  "scope": args.scope,
                  "limits": ["Provider protocol matrix and hostile/stale FILEHOST advertisements belong to backend tests.",
                             "Cancel is exercised while the genuine HTTPS response is held after body receipt; "
                             "mid-read cancellation and the already-finished-result race remain transport/UI unit coverage.",
                             "Connected glyph, host icons/source anchor, faint header line and logo optical alignment "
                             "are retained physical screenshots for parent visual review, not pixel-template assertions."]}
        (output / "soju-discovery.json").write_text(json.dumps(lab.filehost_evidence, indent=2) + "\n")
        try:
            proc, window = gui.launch_gui(binary, root / "initial.bmp", env, (1440, 940), logs)
            if args.scope == "network-names":
                report["limits"] = [
                    "Native palette captions and live catalog rename are asserted from real rendered pixels.",
                    "Screenshots retain other visible captions for human review; no font or cosmetic pixel assertions.",
                    "Mentions caption caching is covered by transcript unit regressions, not this native scope.",
                    "This focused scope does not exercise uploads, folding, restart, or every caption surface."]
                report["network_names"] = probe_network_names(
                    proc, window, preferences, config, database, destination, lab)
            elif args.scope in ("soju", "all"):
                first, second, report["soju"] = probe_soju(
                    proc, window, preferences, config, database, destination, lab, observer)
            else:
                first, second, _, channels = discover_children(proc, window, preferences, config, database)
                report["soju_discovery"] = {"live_children": channels}
                gui.switch_buffer("#blastirc-soju-other", second)
                gui.type_text("second independent draft")
                snapshot(proc, window, preferences, database)
                gui.switch_buffer("#blastirc-soju", first)
                gui.clear_draft()
                snapshot(proc, window, preferences, database)
            if args.scope in ("uploads", "all"):
                report["uploads"] = probe_uploads(proc, window, preferences, database, destination,
                                                 services, fixture, file, ime, second)
                report["origin_lifecycle"] = probe_origin_lifecycle(
                    proc, window, preferences, database, destination, fixture, file, observer, first, lab, services)
                gui.switch_buffer("#blastirc-soju", first)
                report["filehost"] = probe_filehost(proc, window, preferences, database, destination, lab, file, services)
            if args.scope != "network-names":
                gui.click(window, *rail_point(preferences, 0))
                persisted = snapshot(proc, window, preferences, database)
                gui.wait_preference(proc, preferences, "workspace", "soju-group:Account", "collapsed")
                assert tomllib.loads(preferences.read_text())["workspace"]["soju-group:Account"] == "collapsed"
            stdout, stderr = gui.finish_gui(proc, window, root / "initial.bmp", destination, (1440, 940))
            logs.append(("initial", stdout, stderr))
            proc = None
            if args.scope != "network-names":
                proc, window = gui.launch_gui(binary, root / "restart.bmp", env, (1440, 940), logs)
                restored = snapshot(proc, window, preferences, database)
                gui.wait_preference(proc, preferences, "workspace", "soju-group:Account", "collapsed")
                assert tomllib.loads(preferences.read_text())["workspace"]["soju-group:Account"] == "collapsed"
                assert restored[1] == persisted[1], "fold restart changed current conversation"
                if args.scope in ("soju", "all"):
                    assert next(profile for profile in saved_profiles(config) if profile["name"] == "Account")["alias"] == "Home"
                    assert {profile["name"]: profile.get("soju_parent") for profile in saved_profiles(config)
                            if profile["name"] in (first, second)} == {first: "Account", second: "Account"}, \
                        "restart changed child parent links"
                    assert_parent_alias_caption(window, preferences, destination, "parent-account-alias-restarted")
                    report["soju"]["parent_alias_restart_preserved"] = True
                if args.scope in ("uploads", "all"):
                    assert tomllib.loads(preferences.read_text())["uploads"]["provider"] == "custom"
                gui.capture_window(window, destination, "persisted-fold")
                stdout, stderr = gui.finish_gui(proc, window, root / "restart.bmp",
                                                output / "restart.bmp", (1440, 940))
                logs.append(("restart", stdout, stderr))
                proc = None
                report["fold_restart_preserved"] = True
            if args.scope in ("uploads", "all"):
                # A fresh private geometry fixture uses the supported minimum viewport at max zoom.
                # The original full gui_smoke.py workflow is untouched.
                uploads = tomllib.loads(preferences.read_text())["uploads"]
                preferences.write_text('[appearance]\nscale = 2.0\nreduced_motion = true\n[uploads]\n' +
                                       "".join(f"{key} = {q(value)}\n" for key, value in uploads.items()))
                gui.SESSION.ipc("command", "output HEADLESS-1 mode --custom 720x480@60Hz")
                gui.SESSION.until("private minimum-size output configured", lambda: gui.display_geometry() == (720, 480))
                proc, window = gui.launch_gui(binary, root / "minimum.bmp", env, (720, 480), logs)
                gui.switch_buffer("#blastirc-soju", first)
                focus_composer(window)
                gui.clear_draft()
                snapshot(proc, window, preferences, database)
                probe_accessory(proc, window, preferences, database, output / "minimum.bmp", ime, zoom=2)
                minimum = output / "minimum.bmp"
                assert fixture.requests.empty(), "unexpected Custom upload before the minimum review"
                choose_host(window, preferences, 2, minimum)
                native_choose(window, file, minimum, services)
                assert fixture.requests.empty(), "native file selection started a provider request without Upload"
                gui.capture_window(window, minimum, "conversation-bounded-review")
                region = upload_panel_region(window)
                if label_rect(window, minimum, "Close", region=region) is None:
                    gui.move_pointer(window, region[0] + region[2] / 2, region[1] + region[3] / 2)
                    gui.POINTER.scroll(20)
                panel_action(window, minimum, "Close")
                gui.SESSION.until("minimum review closes through its native accessible control", lambda:
                                  label_rect(window, minimum, file.name, region=region, multiline=True) is None)
                assert fixture.requests.empty(), "closing minimum review emitted a provider request"
                gui.capture_window(window, minimum, "review-closed")
                report["minimum_720x480_zoom2_review"] = {
                    "file": file.name, "native_close_activated": True, "no_provider_request": True}
                stdout, stderr = gui.finish_gui(proc, window, root / "minimum.bmp",
                                                output / "minimum.bmp", (720, 480))
                logs.append(("minimum", stdout, stderr))
                proc = None
                report["minimum_720x480_zoom2_accessory"] = True
            report["passed"] = True
        finally:
            if proc is not None:
                if proc.poll() is None:
                    gui.capture_window(window, destination, "failed-live")
                gui.stop_group(proc)
                stdout, stderr = gui.collect_gui(proc)
                logs.append(("failure", stdout, stderr))
            def redact(text):
                return lab._redact(text).replace("private-upload-password", "<private-upload-password>")
            for label, stdout, stderr in logs:
                (output / (label + ".wayland.log")).write_text(redact(stderr))
                (output / (label + ".stdout.log")).write_text(redact(stdout))
            report["evidence_dir"] = str(output)
            (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2), flush=True)


if __name__ == "__main__":
    run()
