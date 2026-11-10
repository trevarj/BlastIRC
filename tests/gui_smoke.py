#!/usr/bin/env python3
"""Verify real native Wayland GUI/IRC behavior on an owned headless Sway session."""
import argparse
from contextlib import ExitStack, nullcontext
import ctypes
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import sqlite3
import struct
import tempfile
import threading
import time
import tomllib
from dpi_smoke import HeadlessSway, capture_png, main_surface_ids, protocol_evidence, stop_group, tree_windows

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("blastirc_integration", ROOT / "tests/integration/run.py")
fixtures = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = fixtures
spec.loader.exec_module(fixtures)


class WireObserver(fixtures.BootstrapClient):
    """The existing real TLS observer, retaining the actual tags and wire frames."""

    def __init__(self, *args, **kwargs):
        self.frames = []
        self.last_tags = {}
        self.isupport = {}
        self.capabilities = set()
        super().__init__(*args, **kwargs)

    @staticmethod
    def _tag_value(value):
        escapes = {":": ";", "s": " ", "\\": "\\", "r": "\r", "n": "\n"}
        return re.sub(r"\\(.)?", lambda match: escapes.get(match[1], match[1]) if match[1] is not None else "", value)

    def read(self, deadline):
        while True:
            self.socket.settimeout(max(.01, deadline - time.monotonic()))
            frame = self.stream.readline(16385)
            if not frame:
                raise RuntimeError("bootstrap daemon closed the IRC connection")
            if len(frame) > 16384 or not frame.endswith(b"\n"):
                raise RuntimeError("oversized bootstrap IRC frame")
            line = frame.decode("utf-8", errors="replace").rstrip("\r\n")
            tags = {}
            if line.startswith("@"):
                raw, line = line[1:].split(" ", 1)
                for item in raw.split(";"):
                    name, separator, value = item.partition("=")
                    tags[name] = self._tag_value(value) if separator else None
            prefix = ""
            if line.startswith(":"):
                prefix, line = line[1:].split(" ", 1)
            head, separator, trailing = line.partition(" :")
            parts = head.split()
            if separator:
                parts.append(trailing)
            command, params = parts[0], parts[1:]
            self.frames.append({"command": command, "params": params,
                                "prefix": prefix, "tags": tags})
            if command == "005":
                for item in params[1:-1]:
                    name, separator, value = item.partition("=")
                    if name.startswith("-"):
                        self.isupport.pop(name[1:], None)
                    else:
                        self.isupport[name] = value if separator else None
            if command == "CAP" and "ACK" in params:
                for item in params[-1].split():
                    name = item.lstrip("-").split("=", 1)[0]
                    if item.startswith("-"):
                        self.capabilities.discard(name)
                    else:
                        self.capabilities.add(name)
            if command == "PING":
                self.send("PONG :" + params[-1])
                continue
            if command == "ERROR" or command in {"464", "465", "904", "905", "906"}:
                raise RuntimeError("fixture registration rejected: " + command)
            self.last_tags = tags
            return command, params, prefix


def client_tags_allowed(observer, *names):
    import fnmatch
    if "message-tags" not in observer.capabilities:
        return False
    rules = (observer.isupport.get("CLIENTTAGDENY") or "").split(",")
    for name in names:
        name = name.removeprefix("+")
        denied = any(not rule.startswith("-") and fnmatch.fnmatchcase(name, rule)
                     for rule in rules)
        allowed = any(rule.startswith("-") and fnmatch.fnmatchcase(name, rule[1:])
                      for rule in rules)
        if denied and not allowed:
            return False
    return True

GUI_SIZE = None
SESSION = None
POINTER = None
GUI_DEBUG = {}
CHANNEL_ID = QUERY_ID = None
SETTINGS_OPEN = {}  # Coordinate bookkeeping only; never an opening/dismissal oracle.


class _WaylandInterface(ctypes.Structure):
    pass


class _WaylandMessage(ctypes.Structure):
    _fields_ = [("name", ctypes.c_char_p), ("signature", ctypes.c_char_p),
                ("types", ctypes.POINTER(ctypes.POINTER(_WaylandInterface)))]


_WaylandInterface._fields_ = [
    ("name", ctypes.c_char_p), ("version", ctypes.c_int),
    ("method_count", ctypes.c_int), ("methods", ctypes.POINTER(_WaylandMessage)),
    ("event_count", ctypes.c_int), ("events", ctypes.POINTER(_WaylandMessage)),
]


class _WaylandArgument(ctypes.Union):
    _fields_ = [("i", ctypes.c_int32), ("u", ctypes.c_uint32), ("f", ctypes.c_int32),
                ("s", ctypes.c_char_p), ("o", ctypes.c_void_p), ("n", ctypes.c_uint32),
                ("a", ctypes.c_void_p), ("h", ctypes.c_int32)]


def native_library(package, soname):
    directory = subprocess.run(["pkg-config", "--variable=libdir", package],
                               check=True, capture_output=True, text=True, timeout=5).stdout.strip()
    assert Path(directory).is_absolute(), (package, directory)
    return ctypes.CDLL(str(Path(directory) / soname))


class VirtualSeat:
    """Keep genuine pointer/keyboard capabilities alive across wtype invocations."""

    def __init__(self, socket):
        self.lib = native_library("wayland-client", "libwayland-client.so.0")
        self.display = self.registry = self.manager = self.pointer = None
        self.seat = self.keyboard_manager = self.keyboard = None
        self.global_id = self.seat_id = self.keyboard_manager_id = None
        self.capabilities = 0
        self.lib.wl_display_connect.argtypes = [ctypes.c_char_p]
        self.lib.wl_display_connect.restype = ctypes.c_void_p
        self.lib.wl_display_roundtrip.argtypes = [ctypes.c_void_p]
        self.lib.wl_display_roundtrip.restype = ctypes.c_int
        self.lib.wl_display_disconnect.argtypes = [ctypes.c_void_p]
        self.lib.wl_display_disconnect.restype = None
        self.lib.wl_proxy_destroy.argtypes = [ctypes.c_void_p]
        self.lib.wl_proxy_destroy.restype = None
        self.lib.wl_proxy_add_listener.argtypes = [
            ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p), ctypes.c_void_p]
        self.lib.wl_proxy_add_listener.restype = ctypes.c_int
        self.lib.wl_proxy_marshal_array_flags.argtypes = [
            ctypes.c_void_p, ctypes.c_uint32, ctypes.POINTER(_WaylandInterface),
            ctypes.c_uint32, ctypes.c_uint32, ctypes.POINTER(_WaylandArgument)]
        self.lib.wl_proxy_marshal_array_flags.restype = ctypes.c_void_p
        self.registry_interface = _WaylandInterface.in_dll(self.lib, "wl_registry_interface")
        self.seat_interface = _WaylandInterface.in_dll(self.lib, "wl_seat_interface")
        # Wire declarations from wlr-protocols/unstable/wlr-virtual-pointer-unstable-v1.xml:
        # https://gitlab.freedesktop.org/wlroots/wlr-protocols
        self.pointer_methods = (_WaylandMessage * 9)(*[
            _WaylandMessage(name.encode(), signature.encode(), None)
            for name, signature in (("motion", "uff"), ("motion_absolute", "uuuuu"),
                                    ("button", "uuu"), ("axis", "uuf"), ("frame", ""),
                                    ("axis_source", "u"), ("axis_stop", "uu"),
                                    ("axis_discrete", "uufi"), ("destroy", ""))])
        self.pointer_interface = _WaylandInterface(
            b"zwlr_virtual_pointer_v1", 1, 9, self.pointer_methods, 0, None)
        self.manager_types = (ctypes.POINTER(_WaylandInterface) * 2)(
            ctypes.pointer(self.seat_interface), ctypes.pointer(self.pointer_interface))
        self.manager_methods = (_WaylandMessage * 2)(
            _WaylandMessage(b"create_virtual_pointer", b"?on", self.manager_types),
            _WaylandMessage(b"destroy", b"", None))
        self.manager_interface = _WaylandInterface(
            b"zwlr_virtual_pointer_manager_v1", 1, 2, self.manager_methods, 0, None)
        # Hold a genuine keyboard and one persistent compose-test map; wtype supplies other keys.
        # https://github.com/atx/wtype/blob/master/protocol/virtual-keyboard-unstable-v1.xml
        self.keyboard_methods = (_WaylandMessage * 4)(*[
            _WaylandMessage(name.encode(), signature.encode(), None)
            for name, signature in (("keymap", "uhu"), ("key", "uuu"),
                                    ("modifiers", "uuuu"), ("destroy", ""))])
        self.keyboard_interface = _WaylandInterface(
            b"zwp_virtual_keyboard_v1", 1, 4, self.keyboard_methods, 0, None)
        self.keyboard_manager_types = (ctypes.POINTER(_WaylandInterface) * 2)(
            ctypes.pointer(self.seat_interface), ctypes.pointer(self.keyboard_interface))
        self.keyboard_manager_methods = (_WaylandMessage * 1)(
            _WaylandMessage(b"create_virtual_keyboard", b"on", self.keyboard_manager_types))
        self.keyboard_manager_interface = _WaylandInterface(
            b"zwp_virtual_keyboard_manager_v1", 1, 1, self.keyboard_manager_methods, 0, None)
        global_callback = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p,
                                          ctypes.c_uint32, ctypes.c_char_p, ctypes.c_uint32)
        remove_callback = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32)
        capabilities_callback = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32)

        def announced(data, registry, name, interface, version):
            if interface == self.manager_interface.name and version >= 1:
                self.global_id = name
            elif interface == self.keyboard_manager_interface.name and version >= 1:
                self.keyboard_manager_id = name
            elif interface == b"wl_seat" and self.seat_id is None:
                self.seat_id = name

        def removed(data, registry, name):
            if name == self.global_id:
                self.global_id = None
            if name == self.seat_id:
                self.seat_id = None
            if name == self.keyboard_manager_id:
                self.keyboard_manager_id = None

        def capabilities(data, seat, caps):
            self.capabilities = caps

        self.capabilities_callback = capabilities_callback(capabilities)
        self.seat_listener = (ctypes.c_void_p * 1)(
            ctypes.cast(self.capabilities_callback, ctypes.c_void_p))

        self.callbacks = (global_callback(announced), remove_callback(removed))
        self.listener = (ctypes.c_void_p * 2)(
            *(ctypes.cast(callback, ctypes.c_void_p) for callback in self.callbacks))
        try:
            # An absolute socket path avoids changing or borrowing os.environ.
            self.display = self.lib.wl_display_connect(os.fsencode(socket))
            assert self.display, f"Cannot connect virtual pointer to {socket}"
            self.registry = self.request(self.display, 1, [_WaylandArgument(o=None)],
                                         self.registry_interface)
            assert self.registry, "Cannot obtain private Wayland registry"
            assert self.lib.wl_proxy_add_listener(self.registry, self.listener, None) == 0
            self.roundtrip()
            assert self.global_id is not None, "Private Sway lacks wlr-virtual-pointer-v1"
            assert self.seat_id is not None and self.keyboard_manager_id is not None, (
                "Private Sway lacks wl_seat or virtual-keyboard-v1")
            self.seat = self.request(self.registry, 0, [
                _WaylandArgument(u=self.seat_id), _WaylandArgument(s=b"wl_seat"),
                _WaylandArgument(u=1), _WaylandArgument(o=None)], self.seat_interface)
            assert self.seat, "Cannot bind the private input seat"
            assert self.lib.wl_proxy_add_listener(self.seat, self.seat_listener, None) == 0
            self.manager = self.request(self.registry, 0, [
                _WaylandArgument(u=self.global_id), _WaylandArgument(s=self.manager_interface.name),
                _WaylandArgument(u=1), _WaylandArgument(o=None)], self.manager_interface)
            assert self.manager, "Cannot bind native virtual pointer manager"
            self.pointer = self.request(self.manager, 0, [
                _WaylandArgument(o=self.seat), _WaylandArgument(o=None)], self.pointer_interface)
            assert self.pointer, "Cannot create native virtual pointer"
            self.keyboard_manager = self.request(self.registry, 0, [
                _WaylandArgument(u=self.keyboard_manager_id),
                _WaylandArgument(s=self.keyboard_manager_interface.name),
                _WaylandArgument(u=1), _WaylandArgument(o=None)], self.keyboard_manager_interface)
            assert self.keyboard_manager, "Cannot bind native virtual keyboard manager"
            self.keyboard = self.request(self.keyboard_manager, 0, [
                _WaylandArgument(o=self.seat), _WaylandArgument(o=None)], self.keyboard_interface)
            assert self.keyboard, "Cannot create persistent native keyboard"
            self.install_keymap()
            self.roundtrip()
            assert self.capabilities & 3 == 3, (
                "Private seat did not retain keyboard and pointer capabilities", self.capabilities)
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def request(self, proxy, opcode, values=(), interface=None, destroy=False):
        arguments = (_WaylandArgument * len(values))(*values) if values else None
        return self.lib.wl_proxy_marshal_array_flags(
            proxy, opcode, ctypes.byref(interface) if interface is not None else None,
            1, int(destroy), arguments)

    def install_keymap(self):
        xkb = native_library("xkbcommon", "libxkbcommon.so.0")
        xkb.xkb_context_new.argtypes = [ctypes.c_int]
        xkb.xkb_context_new.restype = ctypes.c_void_p
        xkb.xkb_keymap_new_from_names.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int]
        xkb.xkb_keymap_new_from_names.restype = ctypes.c_void_p
        xkb.xkb_keymap_get_as_string.argtypes = [ctypes.c_void_p, ctypes.c_int]
        xkb.xkb_keymap_get_as_string.restype = ctypes.c_void_p
        xkb.xkb_keymap_unref.argtypes = [ctypes.c_void_p]
        xkb.xkb_keymap_unref.restype = None
        xkb.xkb_context_unref.argtypes = [ctypes.c_void_p]
        xkb.xkb_context_unref.restype = None
        xkb.xkb_keymap_key_by_name.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        xkb.xkb_keymap_key_by_name.restype = ctypes.c_uint
        xkb.xkb_keymap_key_get_syms_by_level.argtypes = [
            ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint,
            ctypes.POINTER(ctypes.POINTER(ctypes.c_uint))]
        xkb.xkb_keymap_key_get_syms_by_level.restype = ctypes.c_int
        xkb.xkb_keysym_from_name.argtypes = [ctypes.c_char_p, ctypes.c_int]
        xkb.xkb_keysym_from_name.restype = ctypes.c_uint
        class RuleNames(ctypes.Structure):
            _fields_ = [(name, ctypes.c_char_p) for name in ("rules", "model", "layout", "variant", "options")]
        names = RuleNames(b"evdev", b"pc105", b"us", b"intl", b"")
        libc = ctypes.CDLL(None)
        libc.free.argtypes = [ctypes.c_void_p]
        libc.free.restype = None
        context = xkb.xkb_context_new(0)
        assert context, "Cannot create native XKB context"
        keymap = text = None
        try:
            keymap = xkb.xkb_keymap_new_from_names(context, ctypes.byref(names), 0)
            assert keymap, "Cannot compile the private keyboard's native keymap"
            text = xkb.xkb_keymap_get_as_string(keymap, 1)
            assert text, "Cannot serialize the private keyboard's keymap"
            self.keymap_data = ctypes.string_at(text) + b"\0"
            self.compose_keys = {}
            for name, physical, symbol in (("dead_acute", b"AC11", b"dead_acute"),
                                           ("e", b"AD03", b"e"), ("BackSpace", b"BKSP", b"BackSpace")):
                code = xkb.xkb_keymap_key_by_name(keymap, physical)
                symbols = ctypes.POINTER(ctypes.c_uint)()
                count = xkb.xkb_keymap_key_get_syms_by_level(keymap, code, 0, 0, ctypes.byref(symbols))
                assert code >= 8 and count == 1 and symbols[0] == xkb.xkb_keysym_from_name(symbol, 0), name
                self.compose_keys[name] = code - 8
            self.publish_keymap()
        finally:
            if text:
                libc.free(text)
            if keymap:
                xkb.xkb_keymap_unref(keymap)
            xkb.xkb_context_unref(context)

    def publish_keymap(self):
        fd = os.memfd_create("blastirc-smoke-keymap", os.MFD_CLOEXEC)
        try:
            with os.fdopen(fd, "wb", closefd=False) as stream:
                stream.write(self.keymap_data)
            self.request(self.keyboard, 0, [
                _WaylandArgument(u=1), _WaylandArgument(h=fd),
                _WaylandArgument(u=len(self.keymap_data))])
            self.roundtrip()
        finally:
            os.close(fd)

    def keyboard_key(self, name):
        code = self.compose_keys[name]
        for state in (1, 0):
            self.request(self.keyboard, 1, [
                self.timestamp(), _WaylandArgument(u=code), _WaylandArgument(u=state)])
        self.roundtrip()
        return code

    def roundtrip(self):
        assert self.lib.wl_display_roundtrip(self.display) >= 0, "Private virtual input disconnected"

    def frame(self):
        assert self.global_id is not None, "Native virtual pointer manager was removed"
        self.request(self.pointer, 4)
        self.roundtrip()

    @staticmethod
    def timestamp():
        return _WaylandArgument(u=int(time.monotonic() * 1000) & 0xffffffff)

    def move(self, x, y, width, height):
        self.request(self.pointer, 1, [self.timestamp(), _WaylandArgument(u=round(x)),
                     _WaylandArgument(u=round(y)), _WaylandArgument(u=width),
                     _WaylandArgument(u=height)])
        self.frame()

    def button(self, pressed, code=0x110):
        self.request(self.pointer, 2, [self.timestamp(), _WaylandArgument(u=code),
                                      _WaylandArgument(u=int(pressed))])
        self.frame()

    def scroll(self, steps):
        self.request(self.pointer, 5, [_WaylandArgument(u=0)])  # Wheel source.
        self.request(self.pointer, 7, [self.timestamp(), _WaylandArgument(u=0),
                     _WaylandArgument(f=steps * 15 * 256), _WaylandArgument(i=steps)])
        self.frame()

    def __enter__(self):
        return self

    def __exit__(self, *unused):
        if self.display:
            if self.keyboard:
                self.request(self.keyboard, 3, destroy=True)
            if self.keyboard_manager:
                self.lib.wl_proxy_destroy(self.keyboard_manager)
            if self.pointer:
                self.request(self.pointer, 8, destroy=True)
            if self.manager:
                self.request(self.manager, 1, destroy=True)
            if self.seat:
                self.lib.wl_proxy_destroy(self.seat)
            if self.registry:
                self.lib.wl_proxy_destroy(self.registry)
            self.lib.wl_display_disconnect(self.display)
            self.display = self.pointer = self.manager = self.registry = None
            self.seat = self.keyboard = self.keyboard_manager = None


def window_size(value):
    try:
        width, height = (int(part) for part in value.lower().split("x"))
    except ValueError:
        raise argparse.ArgumentTypeError("--size must be WIDTHxHEIGHT") from None
    if width < 720 or height < 480:
        raise argparse.ArgumentTypeError("--size must be at least 720x480")
    return width, height


def key(*keys, trace=False):
    argv = ["wtype"]
    for chord in keys:
        *modifiers, symbol = chord.split("+")
        # wtype installs the requested keysym literally, without a physical
        # keyboard's shifted Tab level. Native reverse traversal is ISO_Left_Tab.
        if symbol == "Tab" and "shift" in modifiers:
            symbol = "ISO_Left_Tab"
        for modifier in modifiers:
            argv += ["-M", modifier]
        argv += ["-k", symbol]
        for modifier in reversed(modifiers):
            argv += ["-m", modifier]
    result = subprocess.run(argv, env=SESSION.env | {"WAYLAND_DEBUG": "client"} if trace else SESSION.env,
                            capture_output=trace, check=True, timeout=10)
    if "ctrl+g" in keys:
        for window in SETTINGS_OPEN:
            SETTINGS_OPEN[window] = False
    if trace:
        presses = [int(code) for code in re.findall(
            rb"zwp_virtual_keyboard_v1[@#]\d+\.key\(\d+,\s*(\d+),\s*1\)", result.stderr)]
        assert presses, ("Virtual keyboard emitted no requested key presses", result.stderr.decode(errors="replace"))
        return presses


def type_text(text):
    subprocess.run(["wtype", "-d", "15", "--", text], env=SESSION.env, check=True, timeout=15)


def clipboard_text():
    result = subprocess.run(["wl-paste", "--no-newline", "--type", "text/plain;charset=utf-8"],
                            env=SESSION.env, capture_output=True, timeout=5)
    # Preserve the actual clipboard CRLF bytes for the native normalization probe.
    return result.stdout.decode("utf-8") if result.returncode == 0 else None


def seed_clipboard(text, cleanup):
    process = subprocess.Popen(["wl-copy", "--foreground", "--type", "text/plain;charset=utf-8"],
                               env=SESSION.env, stdin=subprocess.PIPE,
                               stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                               start_new_session=True)
    cleanup.callback(stop_group, process)
    process.stdin.write(text.encode("utf-8"))
    process.stdin.close()
    SESSION.until("native clipboard ownership", lambda: clipboard_text() == text)


def _palette_query(window, text, *, after=None):
    _, path, root = GUI_DEBUG[window]
    entered = None
    for event in re.finditer(
            r"zwp_text_input_v3[@#]\d+\.(enter|leave)\(wl_surface[@#](\d+)\)",
            path.read_text(errors="replace")):
        entered = int(event[2]) if event[1] == "enter" else None
    if entered != root:
        # NO-IME deliberately has no surrounding-text channel. The shortcut already
        # has a root paint fence; after literal typing, deliver real cursor keys and
        # fence their paint without touching the clipboard or changing query text.
        if text:
            framed_keys(window, "Left", "Right")
        return
    if after is not None:
        SESSION.until("fresh native palette Input enable", lambda:
                      re.search(r"zwp_text_input_v3[@#]\d+\.enable\(\)",
                                path.read_bytes()[after:].decode(errors="replace")))
    length = len(text.encode("utf-8"))
    geometry = window_geometry(window)
    preferences = Path(SESSION.env["XDG_CONFIG_HOME"]) / "blastirc/settings.toml"
    scale = tomllib.loads(preferences.read_text())["appearance"]["scale"]
    width = min(620 * scale, geometry["WIDTH"] - 20 * scale)
    height = min(480 * scale, geometry["HEIGHT"] - 30 * scale)
    left, top = (geometry["WIDTH"] - width) / 2, (geometry["HEIGHT"] - height) / 2

    def observed():
        state = native_input_state(window)
        if state is None or state["caret"] is None:
            return False
        x, y, caret_width, caret_height = state["caret"]
        in_query = (left + 16 * scale <= x and x + caret_width <= left + width - 16 * scale and
                    top + 30 * scale <= y and y + caret_height <= top + 104 * scale)
        if not text:
            # Column padding scales; native Input's own horizontal inset does not.
            in_query &= left + 16 * scale + 4 <= x <= left + 16 * scale + 32
        return in_query and (state["text"], state["cursor"], state["anchor"]) == (text, length, length)

    SESSION.until("exact native palette query transaction", observed)


def _open_native_palette(*shortcuts, window=None):
    if window is None:
        live = [identifier for identifier, trace in GUI_DEBUG.items() if trace[0].poll() is None]
        assert len(live) == 1, ("Expected one owned GUI for native palette entry", live)
        window = live[0]
    proc, path, root = GUI_DEBUG[window]

    def root_has_keyboard():
        assert proc.poll() is None, "GUI exited before native palette entry"
        surface = None
        for event in re.finditer(
                r"wl_keyboard[@#]\d+\.(enter|leave)\(\d+,\s*wl_surface[@#](\d+)",
                path.read_text(errors="replace")):
            surface = int(event[2]) if event[1] == "enter" else None
        return surface == root

    # A popup's root paint can precede its keyboard-focus return. Never type a
    # command or buffer name until the real root receives its shortcut and opens an Input.
    SESSION.until("native keyboard focus returns to the owned root", root_has_keyboard)
    marker = path.stat().st_size
    presses = key(*shortcuts, trace=True)
    final = presses[-1]
    await_native_frame(window, marker,
                       re.compile(rf"wl_keyboard[@#]\d+\.key\(\d+,\s*\d+,\s*{final},\s*1\)"),
                       presses.count(final))
    _palette_query(window, "", after=marker)
    return window


def command(name, window=None):
    target = _open_native_palette("alt+x", window=window)
    type_text(name)
    _palette_query(target, name)
    marker = GUI_DEBUG[window][1].stat().st_size if window is not None else None
    key("Return")
    if window is not None:
        await_native_frame(window, marker,
                           re.compile(r"wl_keyboard[@#]\d+\.key\(\d+,\s*\d+,\s*\d+,\s*1\)"))
    return marker




def receive_until(observer, command, params, nick, redact=str):
    observed = []

    def matches(actual, arguments, prefix):
        matched = actual == command and arguments == params and prefix.split("!", 1)[0] == nick
        if actual == "PRIVMSG" and not matched:
            if len(observed) == 20:
                observed.pop(0)
            observed.append((actual, arguments, prefix))
        return matched

    try:
        return observer.wait(matches, timeout=15)
    except TimeoutError as error:
        detail = (f"Did not observe {command} {params!r} from {nick}; "
                  f"actual nonmatching PRIVMSG frames: {observed!r}")
        raise AssertionError(redact(detail)) from error


def wait_joins(observer, channel, nicks):
    pending = set(nicks)
    while pending:
        _, _, prefix = observer.wait(lambda actual, params, prefix:
                                     actual == "JOIN" and params == [channel] and
                                     prefix.split("!", 1)[0] in pending, timeout=15)
        pending.remove(prefix.split("!", 1)[0])


def wait_messages(proc, path, expected):
    """Wait for exact durable messages, including destination, sender and reconciled own echoes."""
    texts = tuple(message[2] for message in expected)
    query = ("SELECT buffer,nick,text,own,pending FROM messages WHERE text IN (" +
             ",".join("?" for _ in texts) + ")")
    database = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    try:
        deadline = time.monotonic() + 15
        while True:
            if proc.poll() is not None:
                raise AssertionError(f"GUI exited {proc.returncode} while receiving messages")
            actual = sorted(database.execute(query, texts).fetchall())
            if actual == sorted(expected):
                return
            if time.monotonic() >= deadline:
                raise AssertionError(f"Persisted messages differ: expected {expected!r}, got {actual!r}")
            time.sleep(0.025)  # Observe real receipt/echo reconciliation, not editor timing.
    finally:
        database.close()


def wait_read_marker(proc, path, buffer_id):
    """Observe the focused bottom-follow pane acknowledging its newest real server-time."""
    database = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    try:
        deadline = time.monotonic() + 15
        while True:
            if proc.poll() is not None:
                raise AssertionError(f"GUI exited {proc.returncode} while settling read marker")
            actual = database.execute(
                "SELECT b.read_marker,b.unread,m.id,m.text,m.time FROM buffers AS b "
                "JOIN messages AS m ON m.buffer=b.id WHERE b.id=? AND m.pending=0 AND m.presence=0 "
                "ORDER BY m.sort_time DESC,m.seq DESC LIMIT 1", (buffer_id,),
            ).fetchone()
            if actual and actual[4] and actual[0] == actual[4] and actual[1] == 0:
                return actual
            if time.monotonic() >= deadline:
                raise AssertionError(
                    f"Focused read marker for {buffer_id} did not cover newest actual message: "
                    f"(read_marker,unread,id,text,server_time)={actual!r}")
            time.sleep(0.025)  # Observe the native automatic-read action, not editor timing.
    finally:
        database.close()


def wait_rows(proc, path, query, params, expected):
    database = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    try:
        deadline = time.monotonic() + 15
        while True:
            if proc.poll() is not None:
                raise AssertionError(f"GUI exited {proc.returncode} while persisting state")
            actual = database.execute(query, params).fetchall()
            if actual == expected:
                return
            if time.monotonic() >= deadline:
                raise AssertionError(f"Persisted state differs: expected {expected!r}, got {actual!r}")
            time.sleep(0.025)
    finally:
        database.close()


def native_window(window):
    matches = [node for node, _ in tree_windows(SESSION.ipc("get_tree")) if node["id"] == window]
    assert len(matches) == 1, (window, matches)
    return matches[0]


def window_geometry(window):
    global GUI_SIZE
    node = native_window(window)
    outer, client = node["rect"], node["window_rect"]
    GUI_SIZE = client["width"], client["height"]
    return {"X": outer["x"] + client["x"], "Y": outer["y"] + client["y"],
            "WIDTH": client["width"], "HEIGHT": client["height"]}


def display_geometry():
    outputs = [output for output in SESSION.ipc("get_outputs") if output["active"]]
    assert len(outputs) == 1, outputs
    return outputs[0]["rect"]["width"], outputs[0]["rect"]["height"]


def read_layout(path):
    """Derive the direct shell allocation; persisted state contains no geometry."""
    settings = tomllib.loads(path.read_text())
    workspace = settings["workspace"]
    active_buffer = workspace["visible_buffers"]
    live = [window for window, trace in GUI_DEBUG.items() if trace[0].poll() is None]
    assert len(live) <= 1, live
    if live:
        window_geometry(live[0])
    width, height = GUI_SIZE
    scale = settings["appearance"]["scale"]
    compact = height < 600 * scale or width < 1000 * scale
    tight = height < 360 * scale
    extent = max(32, (32 if compact else 34) * scale)
    title_height = extent + (10 if tight else 16 if compact else 20) * scale
    status_height = (22 if tight else 26 if compact else 30) * scale
    outer = (4 if tight else 8 if compact else 12) * scale
    rail_width = float(workspace.get("server_rail_width", 76)) * scale
    top = title_height + outer
    content_height = max(1, height - top - status_height - outer)
    navigation_intent = workspace["native_navigation_mode"]
    navigation = navigation_intent
    database_path = next(reversed(GUI_DEBUG.values()))[0].native_database_path
    people = workspace["people_mode"] if database_buffer_kind(
        database_path, active_buffer) == "channel" else "hidden"
    composer_minimum = max(48, 32 * scale) + 2 * max(32, 32 * scale) + 16 * scale
    chat_minimum = max(360 * scale, composer_minimum + 24 * scale)
    compact_width = float(workspace.get("compact_channel_sidebar_width", 56)) * scale
    if "server_rail_width" in workspace:
        rail_width = min(rail_width, max(56 * scale,
            width - 4 * outer - compact_width - chat_minimum))
    rail_width = min(rail_width, max(0, width - 2 * outer))
    widths = {"expanded": float(workspace.get("channel_sidebar_width", 250)) * scale,
              "compact": compact_width, "hidden": 0}
    people_widths = {"expanded": 220 * scale, "compact": 56 * scale, "hidden": 0}
    remaining = max(1, width - 2 * outer - rail_width - outer)

    def available():
        return (remaining - widths[navigation] - (outer if navigation != "hidden" else 0)
                - people_widths[people] - (outer if people != "hidden" else 0))

    if available() < chat_minimum and people == "expanded":
        people = "compact"
    if available() < chat_minimum and navigation == "expanded":
        navigation = "compact"
    if available() < chat_minimum and people == "compact":
        people = "hidden"
    sidebar_width, people_width = widths[navigation], people_widths[people]
    sidebar_x = outer + rail_width + outer
    left = sidebar_x + sidebar_width + (outer if navigation != "hidden" else 0)
    right = width - outer - people_width - (outer if people != "hidden" else 0)
    chat = (left, top, max(1, right - left), content_height)
    layout = {"compact": compact, "tight": tight, "scale": scale,
              "topbar": {"rect": (0, 0, width, title_height)},
              "navigation_intent": navigation_intent,
              "active_buffer": active_buffer,
              "conversation": {"rect": chat, "buffer": active_buffer},
              "rail": {"rect": (outer, top, rail_width, content_height)},
              "channels": {"rect": (sidebar_x, top, sidebar_width, content_height),
                           "mode": navigation},
              "people": {"rect": (right + (outer if people != "hidden" else 0),
                                  top, people_width, content_height), "mode": people}}
    if live and SETTINGS_OPEN.get(live[0], False):
        # Intended geometry only locates input. Actual frame/focus, popup and
        # Save results prove interaction; stale disk diagnostics do not.
        dialog_width = min(1000 * scale, max(0, width - 24))
        dialog_height = min(720 * scale, max(0, height - 24))
        layout["settings"] = {"rect": ((width - dialog_width) / 2,
            (height - dialog_height) / 2, dialog_width, dialog_height)}
    return layout


def shell_actions(layout):
    zoom = layout["scale"]
    _, _, width, height = layout["topbar"]["rect"]
    extent = max(32, (32 if layout["compact"] else 34) * zoom)
    outer, gap = layout["rail"]["rect"][0], 8 * zoom
    left = width - outer - 3 * extent - 2 * gap
    x, y, rail_width, rail_height = layout["rail"]["rect"]
    footer_extent, padding = max(32, 36 * zoom), 6 * zoom
    footer_top = max(padding, rail_height - padding - 2 * footer_extent - 6 * zoom)
    return {"navigation": (left + extent / 2, height / 2),
            "palette": (left + 1.5 * extent + gap, height / 2),
            "settings": (x + rail_width / 2, y + footer_top + 1.5 * footer_extent + 6 * zoom)}


def open_settings(window, preferences, *, from_icon=False):
    """Use the existing native Settings command without requiring a saved workspace."""
    if from_icon:
        marker = GUI_DEBUG[window][1].stat().st_size
        click(window, *shell_actions(read_layout(preferences))["settings"])
        await_native_frame(window, marker,
                           re.compile(r"wl_pointer[@#]\d+\.button\(\d+,\s*\d+,\s*272,\s*0\)"))
    else:
        command("settings", window=window)
    # Bookkeeping must follow the real rendered modal, not merely a queued command.
    # The compact header intentionally shows its category instead of "Settings".
    from PIL import Image
    scale = tomllib.loads(preferences.read_text())["appearance"]["scale"]
    geometry = window_geometry(window)
    width, height = min(1000 * scale, geometry["WIDTH"] - 24), min(720 * scale, geometry["HEIGHT"] - 24)
    compact = width < 760 * scale or height < 440 * scale
    padding = (8 if compact else 12) * scale
    left, top = (geometry["WIDTH"] - width) / 2 + padding, (geometry["HEIGHT"] - height) / 2 + padding
    with tempfile.TemporaryDirectory(prefix="blastirc-settings-caption-", dir=SESSION.env["XDG_RUNTIME_DIR"]) as temporary:
        caption = Path(temporary) / "caption.png"

        def visible_caption():
            captured = native_rgb_capture(window)
            image = Image.frombytes("RGB", (captured["width"], captured["height"]), captured["pixels"])
            image.crop((round(left), round(top), round(left + width - 2 * padding),
                        round(top + min(64 * scale, height - 2 * padding)))).save(caption)
            rendered = subprocess.run(["tesseract", str(caption), "stdout", "--psm", "6"],
                                      check=True, capture_output=True, text=True, timeout=15)
            words = set(re.findall(r"[a-z]+", rendered.stdout.casefold()))
            expected = {"appearance", "keyboard", "chat", "effects", "sounds", "networks", "identities"} if compact else {"settings"}
            return bool(words & expected)

        SESSION.until("actual rendered Settings caption", visible_caption)
    SETTINGS_OPEN[window] = True


def wait_layout(proc, path, required=("channels",), predicate=None):
    deadline = time.monotonic() + 15
    while True:
        if proc.poll() is not None:
            raise AssertionError(f"GUI exited {proc.returncode} while saving native layout")
        try:
            layout = read_layout(path)
            if set(required) <= layout.keys() and (predicate is None or predicate(layout)):
                return layout
        except (FileNotFoundError, KeyError, tomllib.TOMLDecodeError):
            pass
        if time.monotonic() >= deadline:
            raise AssertionError(f"Native workspace never recorded {required!r} satisfying {predicate!r}")
        time.sleep(0.025)  # Observe the real workspace persistence, not injected-input timing.


def sidebar_expanded(layout):
    return layout["channels"]["mode"] == "expanded"


def expand_navigation(proc, window, layout_path, database_path):
    return set_navigation(proc, window, layout_path, database_path, "expanded", "manual-expand")


def log_navigation(label, before, after, window):
    def geometry(layout):
        return {"sidebar_expanded": sidebar_expanded(layout),
                "rects": {name: values for name, values in layout.items()
                          if name in ("channels", "rail", "people", "conversation")}}
    print("Native navigation geometry:", json.dumps({
        "label": label, "before": geometry(before), "after": geometry(after),
        "client": window_geometry(window), "display": display_geometry(),
    }), flush=True)


def set_navigation(proc, window, layout_path, database_path, mode, label):
    assert mode in ("expanded", "compact", "hidden"), mode
    save_state(proc, layout_path, database_path)
    before = after = wait_layout(proc, layout_path)
    for _ in range(3):
        if after["navigation_intent"] == mode:
            log_navigation(label, before, after, window)
            return after
        marker = GUI_DEBUG[window][1].stat().st_size
        click(window, *shell_actions(after)["navigation"])
        await_native_frame(window, marker,
                           re.compile(r"wl_pointer[@#]\d+\.button\(\d+,\s*\d+,\s*272,\s*0\)"))
        save_state(proc, layout_path, database_path)
        after = read_layout(layout_path)
    assert after["navigation_intent"] == mode, (
        "Navigation intent unreachable at current viewport", mode, before, after)
    log_navigation(label, before, after, window)
    return after


def set_people(proc, window, preferences, database_path, mode):
    assert mode in ("expanded", "compact", "hidden"), mode
    for _ in range(3):
        state = save_state(proc, preferences, database_path)
        if state[0]["people_mode"] == mode:
            return state
        layout = read_layout(preferences)
        marker = GUI_DEBUG[window][1].stat().st_size
        click(window, *title_actions(layout, layout["scale"])["people"])
        await_native_frame(window, marker,
                           re.compile(r"wl_pointer[@#]\d+\.button\(\d+,\s*\d+,\s*272,\s*0\)"))
    state = save_state(proc, preferences, database_path)
    assert state[0]["people_mode"] == mode, ("People intent unreachable", mode, state)
    return state


def resize_panel_viewport(window, size):
    if (window_geometry(window)["WIDTH"], window_geometry(window)["HEIGHT"]) == tuple(size):
        return
    debug = GUI_DEBUG[window][1]
    marker, ids = debug.stat().st_size, main_surface_ids(debug)
    SESSION.ipc("command", f"output HEADLESS-1 mode --custom {size[0]}x{size[1]}@60Hz")
    SESSION.until("actual panel viewport configure", lambda:
                  (window_geometry(window)["WIDTH"], window_geometry(window)["HEIGHT"]) == tuple(size))
    SESSION.until("committed native panel viewport", lambda:
                  protocol_evidence(debug, ids, marker, tuple(size), 1))


def probe_three_way_panels(proc, window, preferences, database_path, args, destination, channel_id):
    """Exercise actual shell buttons and Avy; derived rectangles only locate input."""
    # Both callers acknowledge the channel before this retention-only probe begins.
    wait_read_marker(proc, database_path, channel_id)
    entry = save_state(proc, preferences, database_path)
    original_settings = saved_workspace_intent(tomllib.loads(preferences.read_text()))
    original_size = window_geometry(window)
    size = original_size["WIDTH"], original_size["HEIGHT"]
    original_input = native_input_state(window)
    assert original_input is not None, original_input
    select_native_buffer(proc, window, preferences, database_path, args, channel_id)
    current = native_input_state(window)
    seeded = not current["text"]
    if seeded:
        seed = "Three-way panels retain this unsent draft."
        type_text(seed)
        current = _composer_native(window, seed, len(seed.encode("utf-8")), len(seed.encode("utf-8")))
    text, cursor, anchor = current["text"], current["cursor"], current["anchor"]
    assert text, current
    wide = (max(size[0], math.ceil(1300 * args.zoom)), max(size[1], math.ceil(700 * args.zoom)))
    resize_panel_viewport(window, wide)
    set_navigation(proc, window, preferences, database_path, "expanded", "cycles-entry")
    set_people(proc, window, preferences, database_path, "expanded")
    baseline = save_state(proc, preferences, database_path)
    evidence = []

    def retain(label, navigation, people):
        editing = _composer_native(window, text, cursor, anchor)
        state = save_state(proc, preferences, database_path)
        expected = ({**baseline[0], "native_navigation_mode": navigation, "people_mode": people},
                    baseline[1], baseline[2])
        assert_same_state(label + " retains active/read/draft and independent panel intent", expected, state)
        evidence.append({"phase": label, "workspace": state[0], "buffers": state[2],
                         "native_input": editing, "client": window_geometry(window),
                         "capture": str(capture_window(window, destination, "panels-" + label))})

    retain("expanded-entry", "expanded", "expanded")
    for side in ("left", "right"):
        for mode in ("compact", "hidden", "expanded"):
            layout = read_layout(preferences)
            point = (shell_actions(layout)["navigation"] if side == "left" else
                     title_actions(layout, args.zoom)["people"])
            marker = GUI_DEBUG[window][1].stat().st_size
            click(window, *point)
            await_native_frame(window, marker,
                               re.compile(r"wl_pointer[@#]\d+\.button\(\d+,\s*\d+,\s*272,\s*0\)"))
            retain(side + "-" + mode, mode if side == "left" else "expanded",
                   mode if side == "right" else "expanded")
            if side == "left" and mode in ("compact", "hidden"):
                # The passive compact/hidden chat identity must not take text focus.
                metrics = pane_metrics(read_layout(preferences), args.zoom)
                click(window, metrics["x"] + 12 * args.zoom,
                      metrics["y"] + metrics["header_minimum"] / 2)
                retain(side + "-" + mode + "-header-focus", mode, "expanded")
                if mode == "hidden":
                    select_server(proc, window, preferences, database_path, args, "Local")
                    retain("hidden-left-rail-selection", "hidden", "expanded")

    set_navigation(proc, window, preferences, database_path, "hidden", "avy-hidden")
    hidden = save_state(proc, preferences, database_path)
    hidden_preferences = saved_workspace_intent(tomllib.loads(preferences.read_text()))
    database = sqlite3.connect(f"file:{database_path}?mode=ro", uri=True)
    try:
        targets = database.execute(
            "SELECT id,target FROM buffers WHERE server=? AND kind IN ('channel','query') ORDER BY id",
            (hidden[0]["selected_server"],)).fetchall()
    finally:
        database.close()
    # The controlled fixture channel has a unique ASCII initial; never assume list order.
    initials = [(identity, re.search("[a-z0-9]", target.lower()).group()) for identity, target in targets]
    hint = next(initial for identity, initial in initials if identity == channel_id)
    assert sum(initial == hint for _, initial in initials) == 1, initials
    for action in ("cancel", "choose"):
        overlay = open_avy(window, preferences, destination, "hidden-" + action + "-temporary-compact")
        assert saved_workspace_intent(tomllib.loads(preferences.read_text())) == hidden_preferences
        evidence.append({"phase": "hidden-avy-" + action + "-overlay",
                         "workspace": hidden[0], "capture": str(overlay)})
        framed_keys(window, "Escape" if action == "cancel" else hint)
        _composer_native(window, text, cursor, anchor)
        assert_same_state("Hidden Avy " + action + " restores Hidden and read/draft state", hidden,
                          save_state(proc, preferences, database_path))
        retain("hidden-avy-" + action + "-restored", "hidden", "expanded")
        assert saved_workspace_intent(tomllib.loads(preferences.read_text())) == hidden_preferences

    set_navigation(proc, window, preferences, database_path, "expanded", "adaptive-expanded")
    resize_panel_viewport(window, (720, wide[1]))
    regular = read_layout(preferences)
    forced = regular["channels"]["mode"] == "compact"
    retain("responsive-720-" + ("forced-compact" if forced else "expanded-fits") + "-expanded-intent",
           "expanded", "expanded")
    marker = GUI_DEBUG[window][1].stat().st_size
    click(window, *shell_actions(regular)["navigation"])
    await_native_frame(window, marker,
                       re.compile(r"wl_pointer[@#]\d+\.button\(\d+,\s*\d+,\s*272,\s*0\)"))
    # Observe actual regular-mode cycling, not a helper-computed width assertion.
    retain("responsive-720-regular-mode-click", "hidden" if forced else "compact", "expanded")
    set_navigation(proc, window, preferences, database_path, "expanded", "adaptive-restore-intent")
    resize_panel_viewport(window, wide)
    retain("responsive-widen-restores-expanded", "expanded", "expanded")
    marker = GUI_DEBUG[window][1].stat().st_size
    click(window, *shell_actions(read_layout(preferences))["navigation"])
    await_native_frame(window, marker,
                       re.compile(r"wl_pointer[@#]\d+\.button\(\d+,\s*\d+,\s*272,\s*0\)"))
    retain("responsive-widen-regular-expanded-click", "compact", "expanded")
    # Restore actual preferred modes while both controls can reach every intent.
    set_people(proc, window, preferences, database_path, entry[0]["people_mode"])
    set_navigation(proc, window, preferences, database_path,
                   entry[0]["native_navigation_mode"], "cycles-restore")
    if seeded:
        clear_draft()
    select_native_buffer(proc, window, preferences, database_path, args, entry[1][0])
    _composer_native(window, original_input["text"], original_input["cursor"], original_input["anchor"])
    resize_panel_viewport(window, size)
    assert_same_state("Three-way probes restore original workspace/drafts/read state", entry,
                      save_state(proc, preferences, database_path))
    assert saved_workspace_intent(tomllib.loads(preferences.read_text())) == original_settings
    destination.with_name(destination.stem + "-panels-three-way.json").write_text(
        json.dumps(evidence, indent=2, ensure_ascii=False) + "\n")




def capture_window(window, destination, label):
    path = destination.with_name(destination.stem + "-" + label + ".png")
    geometry = window_geometry(window)
    region = f'{geometry["X"]},{geometry["Y"]} {geometry["WIDTH"]}x{geometry["HEIGHT"]}'
    subprocess.run(["grim", "-g", region, "-s", str(1), str(path)],
                   env=SESSION.env, check=True, timeout=10)
    image = path.read_bytes()
    assert image[:8] == b"\x89PNG\r\n\x1a\n", path
    pixels = struct.unpack_from(">II", image, 16)
    expected = geometry["WIDTH"], geometry["HEIGHT"]
    assert pixels == expected, (
        f"Native capture clipped: expected full client {expected}, got {pixels}; "
        f"Wayland={SESSION.env['WAYLAND_DISPLAY']} window={window} geometry={geometry}")
    print("Native state screenshot:", path, flush=True)
    return path


def move_pointer(window, x, y):
    geometry = window_geometry(window)
    assert 0 <= x < geometry["WIDTH"] and 0 <= y < geometry["HEIGHT"], (x, y, geometry)
    POINTER.move(geometry["X"] + x, geometry["Y"] + y, *display_geometry())


def pointer_button(pressed):
    POINTER.button(pressed)


def click(window, x, y):
    move_pointer(window, x, y)
    pointer_button(True)
    pointer_button(False)


class NativeInputMethod:
    """A real input-method-v2 client on the private VirtualSeat connection."""

    def __init__(self, seat):
        self.owner = seat
        self.registry = self.manager = self.method = self.manager_id = None
        self.serial = 0
        self.unavailable = False
        self.pending = {"active": False, "surrounding": None, "cause": 0, "content": (0, 0)}
        self.state = self.pending.copy()
        self.trace = []
        # https://github.com/swaywm/wlroots/blob/master/protocol/input-method-unstable-v2.xml
        # All interfaces are version 1. Popup/grab requests are declared, never used.
        self.methods = (_WaylandMessage * 7)(*[
            _WaylandMessage(name.encode(), signature.encode(), None)
            for name, signature in (("commit_string", "s"), ("set_preedit_string", "sii"),
                                    ("delete_surrounding_text", "uu"), ("commit", "u"),
                                    ("get_input_popup_surface", "no"), ("grab_keyboard", "n"),
                                    ("destroy", ""))])
        self.events = (_WaylandMessage * 7)(*[
            _WaylandMessage(name.encode(), signature.encode(), None)
            for name, signature in (("activate", ""), ("deactivate", ""),
                                    ("surrounding_text", "suu"), ("text_change_cause", "u"),
                                    ("content_type", "uu"), ("done", ""), ("unavailable", ""))])
        self.interface = _WaylandInterface(
            b"zwp_input_method_v2", 1, 7, self.methods, 7, self.events)
        self.manager_types = (ctypes.POINTER(_WaylandInterface) * 2)(
            ctypes.pointer(seat.seat_interface), ctypes.pointer(self.interface))
        self.manager_methods = (_WaylandMessage * 2)(
            _WaylandMessage(b"get_input_method", b"on", self.manager_types),
            _WaylandMessage(b"destroy", b"", None))
        self.manager_interface = _WaylandInterface(
            b"zwp_input_method_manager_v2", 1, 2, self.manager_methods, 0, None)
        event = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p)
        surrounding_event = ctypes.CFUNCTYPE(
            None, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p,
            ctypes.c_uint32, ctypes.c_uint32)
        cause_event = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32)
        content_event = ctypes.CFUNCTYPE(
            None, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32)

        def activate(data, proxy):
            self.pending = {"active": True, "surrounding": None, "cause": 0, "content": (0, 0)}
            self.trace.append({"event": "activate"})

        def deactivate(data, proxy):
            self.pending["active"] = False
            self.trace.append({"event": "deactivate"})

        def surrounding(data, proxy, text, cursor, anchor):
            self.pending["surrounding"] = (text.decode("utf-8", errors="replace"), cursor, anchor)
            self.trace.append({"event": "surrounding", "value": self.pending["surrounding"]})

        def cause(data, proxy, value):
            self.pending["cause"] = value
            self.trace.append({"event": "cause", "value": value})

        def content(data, proxy, hint, purpose):
            self.pending["content"] = (hint, purpose)
            self.trace.append({"event": "content", "value": (hint, purpose)})

        def done(data, proxy):
            # done() has NO serial argument; the lifetime event count is commit's serial.
            self.serial = (self.serial + 1) & 0xffffffff
            self.state = self.pending.copy()
            self.trace.append({"event": "done", "serial": self.serial, "state": self.state.copy()})
            self.pending["surrounding"] = None
            self.pending["cause"] = 0

        def unavailable(data, proxy):
            self.unavailable = True
            self.trace.append({"event": "unavailable"})

        self.callbacks = (event(activate), event(deactivate), surrounding_event(surrounding),
                          cause_event(cause), content_event(content), event(done), event(unavailable))
        self.listener = (ctypes.c_void_p * 7)(
            *(ctypes.cast(callback, ctypes.c_void_p) for callback in self.callbacks))
        global_event = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p,
                                       ctypes.c_uint32, ctypes.c_char_p, ctypes.c_uint32)
        remove_event = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32)

        def announced(data, registry, name, interface, version):
            if interface == self.manager_interface.name and version >= 1:
                self.manager_id = name

        def removed(data, registry, name):
            if name == self.manager_id:
                self.manager_id = None

        self.registry_callbacks = (global_event(announced), remove_event(removed))
        self.registry_listener = (ctypes.c_void_p * 2)(
            *(ctypes.cast(callback, ctypes.c_void_p) for callback in self.registry_callbacks))
        try:
            # Own only this registry/manager/method, never the persistent seat or devices.
            self.registry = seat.request(seat.display, 1, [_WaylandArgument(o=None)],
                                         seat.registry_interface)
            assert self.registry, "Cannot obtain private input-method registry"
            assert seat.lib.wl_proxy_add_listener(self.registry, self.registry_listener, None) == 0
            seat.roundtrip()
            self.check()
            self.manager = seat.request(self.registry, 0, [
                _WaylandArgument(u=self.manager_id),
                _WaylandArgument(s=self.manager_interface.name),
                _WaylandArgument(u=1), _WaylandArgument(o=None)], self.manager_interface)
            assert self.manager, "Cannot bind private input-method-v2 manager"
            self.method = seat.request(self.manager, 0, [
                _WaylandArgument(o=seat.seat), _WaylandArgument(o=None)], self.interface)
            assert self.method, "Cannot create private input-method-v2 object"
            assert seat.lib.wl_proxy_add_listener(self.method, self.listener, None) == 0
            self.drain()
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def check(self):
        assert not self.unavailable, (
            "Private input-method-v2 unavailable: another input method owns the seat "
            "or the seat was removed; genuine IME probe cannot run")
        assert self.manager_id is not None, "Private Sway lacks input-method-v2 manager"

    def drain(self):
        self.owner.roundtrip()
        self.check()

    def transaction(self, preedit, begin, end, committed=None):
        self.drain()
        assert self.state["active"], "Private input method has no activate/done batch"
        serial = self.serial
        if committed is not None:
            self.owner.request(self.method, 0, [_WaylandArgument(s=committed.encode("utf-8"))])
        self.owner.request(self.method, 1, [
            _WaylandArgument(s=preedit.encode("utf-8")),
            _WaylandArgument(i=begin), _WaylandArgument(i=end)])
        self.owner.request(self.method, 3, [_WaylandArgument(u=serial)])
        self.drain()
        return serial

    def __enter__(self):
        return self

    def __exit__(self, *unused):
        owned = bool(self.method or self.manager or self.registry)
        if self.method:
            self.owner.request(self.method, 6, destroy=True)
            self.method = None
        if self.manager:
            self.owner.request(self.manager, 1, destroy=True)
            self.manager = None
        if self.registry:
            self.owner.lib.wl_proxy_destroy(self.registry)
            self.registry = None
        if owned:
            self.owner.roundtrip()


def probe_real_ime(window, destination, cleanup, layout, zoom, method=None):
    """Leave a猫🙂 in the focused composer after genuine authoritative IM activation."""
    from PIL import Image, ImageChops

    captures = {}

    def pixels(label, bounds):
        with Image.open(captures[label]) as image:
            return image.convert("RGB").crop(bounds)

    path = GUI_DEBUG[window][1]
    context = NativeInputMethod(POINTER) if method is None else nullcontext(method)
    with context as method:
        def activated():
            method.drain()
            return method.state["active"] and method.state["surrounding"] == ("", 0, 0)

        try:
            SESSION.until("real input-method activate/done for the empty composer", activated)
        except (AssertionError, RuntimeError):
            print("Real input-method activation evidence:", json.dumps({
                "serial": method.serial, "state": method.state, "pending": method.pending,
                "unavailable": method.unavailable, "events": method.trace,
            }), file=sys.stderr, flush=True)
            raise
        initial = native_input_state(window)
        assert initial is not None and (initial["text"], initial["cursor"], initial["anchor"]) == (
            "", 0, 0), ("Real IME probe requires an empty focused composer after activation", initial)
        print("Real input-method activated:", json.dumps({
            "serial": method.serial, "state": method.state, "events": method.trace,
        }), flush=True)
        enabled = re.findall(r"zwp_text_input_v3[@#](\d+)\.enable\(\)", path.read_text(errors="replace"))
        assert enabled, "Actual composer never enabled text-input-v3"
        text_input = rf"zwp_text_input_v3[@#]{enabled[-1]}"
        done = re.compile(rf"{text_input}\.done\((\d+)\)")

        def observe(label, text, feedback=False):
            result = None
            expected = (text, len(text.encode("utf-8")), len(text.encode("utf-8")))

            def observed():
                nonlocal result
                method.drain()
                state = native_input_state(window)
                if (state is None or state["caret"] is None
                        or (state["text"], state["cursor"], state["anchor"]) != expected):
                    return False
                if feedback and method.state["surrounding"] != expected:
                    return False
                result = state
                return True

            SESSION.until("real IME committed surrounding text/cursor/anchor", observed)
            # Rectangles are advertised insertion geometry, not drawn-glyph/clipping proof.
            captures[label] = capture_window(window, destination, label)
            print("Native IME state:", json.dumps({"phase": label, **result}), flush=True)
            return result

        def transaction(label, preedit, begin, end, text, committed=None):
            marker = path.stat().st_size
            serial = method.transaction(preedit, begin, end, committed)
            preedit_event = re.compile(
                rf'{text_input}\.preedit_string\("{re.escape(preedit)}",\s*{begin},\s*{end}\)')
            commit_event = re.compile(rf'{text_input}\.commit_string\("(.*)"\)')
            applied_offset = applied_serial = None

            def applied():
                nonlocal applied_offset, applied_serial
                method.drain()
                seen_preedit = seen_commit = False
                position = marker
                for raw in path.read_bytes()[marker:].splitlines(keepends=True):
                    line = raw.decode("utf-8", errors="replace")
                    if preedit_event.search(line):
                        seen_preedit = True
                    match = commit_event.search(line)
                    if match:
                        assert committed is not None and match[1] == committed, (
                            "Unexpected real IME commit_string in application batch", label, line)
                        seen_commit = True
                    match = done.search(line)
                    if match:
                        if seen_preedit and (seen_commit or committed is None):
                            applied_offset, applied_serial = position, int(match[1])
                            return True
                        seen_preedit = seen_commit = False
                    position += len(raw)
                return False

            SESSION.until("actual application preedit/commit batch followed by text-input-v3.done", applied)
            await_native_frame(window, applied_offset, done)
            result = observe(label, text, feedback=committed is not None)
            print("Native IME transaction:", json.dumps({
                "phase": label, "input_method_serial": serial, "text_input_done": applied_serial,
                "preedit": preedit, "preedit_cursor": [begin, end], "commit_string": committed,
            }), flush=True)
            return result

        empty = observe("ime-initial-empty", "")
        transaction("ime-colon-preedit", ":smle", 5, 5, "")
        transaction("ime-colon-preedit-cancelled", "", 0, 0, "")
        preedit = transaction("ime-ascii-preedit-ab", "ab", 2, 2, "")
        clipboard_payload = "Clipboard must not replace active preedit."
        seed_clipboard(clipboard_payload, cleanup)
        key_marker = path.stat().st_size
        presses = key("ctrl+v", trace=True)
        released = re.compile(
            rf"wl_keyboard[@#]\d+\.key\(\d+,\s*\d+,\s*{presses[-1]},\s*0\)")
        SESSION.until("actual blocked clipboard shortcut release", lambda:
                      released.search(path.read_bytes()[key_marker:].decode("utf-8", errors="replace")))
        # A consumed shortcut need not repaint. Hovering Plus does, without
        # changing focus, text or the composition whose pixels are being checked.
        targets = composer_primary_targets(layout, zoom, preedit["caret"])
        move_pointer(window, *targets["send"])
        paint_marker = path.stat().st_size
        move_pointer(window, *targets["plus"])
        await_native_frame(window, paint_marker, re.compile(r"wl_pointer[@#]\d+\.motion\("))
        clipboard_preedit = observe("ime-ascii-preedit-after-clipboard", "")
        assert method.state["active"] and clipboard_preedit["caret"] == preedit["caret"], (
            "Clipboard shortcut displaced the input-method composition", preedit, clipboard_preedit)
        partial = transaction("ime-ascii-same-visible-partial", "b", 1, 1, "a", committed="a")
        # Absence of commit_string is intentional: an explicit empty commit is different.
        transaction("ime-ascii-cancel-remainder", "", 0, 0, "a")
        transaction("ime-ascii-preedit-b-reference", "b", 1, 1, "a")
        reference = transaction("ime-ascii-committed-ab-reference", "", 0, 0, "ab", committed="b")
        assert preedit["caret"] == partial["caret"] == reference["caret"], (preedit, partial, reference)
        x, y, width, height = empty["caret"]
        top = max(y, reference["caret"][1])
        bottom = min(y + height, reference["caret"][1] + reference["caret"][3])
        row_height = bottom - top
        # Only the interior glyph band: no right-hand caret or lower preedit underline.
        glyph_bounds = (x + max(2, width), top + math.ceil(row_height * .15),
                        reference["caret"][0] - 2, top + math.floor(row_height * .7))
        assert glyph_bounds[0] < glyph_bounds[2] and glyph_bounds[1] < glyph_bounds[3], glyph_bounds
        glyphs = pixels("ime-ascii-committed-ab-reference", glyph_bounds)
        assert ImageChops.difference(glyphs, pixels("ime-initial-empty", glyph_bounds)).getbbox(), (
            "Committed ASCII reference contains no distinguishable glyph ink", glyph_bounds)
        for label in ("ime-ascii-preedit-ab", "ime-ascii-preedit-after-clipboard",
                      "ime-ascii-same-visible-partial"):
            assert ImageChops.difference(glyphs, pixels(label, glyph_bounds)).getbbox() is None, (
                "Actual preedit glyphs differ from the same committed ASCII glyphs", label, glyph_bounds)
        framed_keys(window, "BackSpace")
        committed_a = observe("ime-ascii-committed-a-reference", "a", feedback=True)
        cancel_bounds = (committed_a["caret"][0] + committed_a["caret"][2] + 2, glyph_bounds[1],
                         glyph_bounds[2], glyph_bounds[3])
        assert cancel_bounds[0] < cancel_bounds[2], cancel_bounds
        cancelled_pixels = pixels("ime-ascii-committed-a-reference", cancel_bounds)
        assert ImageChops.difference(
            cancelled_pixels, pixels("ime-ascii-preedit-ab", cancel_bounds)).getbbox(), (
                "Cancellation reference does not cover the removed preedit glyph", cancel_bounds)
        assert ImageChops.difference(
            cancelled_pixels, pixels("ime-ascii-cancel-remainder", cancel_bounds)).getbbox() is None, (
                "Cancelling preedit left visible remainder glyphs", cancel_bounds)
        transaction("ime-unicode-preedit", "猫🙂", 7, 7, "a")
        transaction("ime-unicode-same-visible-partial", "🙂", 4, 4, "a猫", committed="猫")
        finished = transaction("ime-unicode-finished", "", 0, 0, "a猫🙂", committed="🙂")
        shown = transaction("ime-visible-composing-cursor", "猫", 3, 3, "a猫🙂")
        hidden = transaction("ime-hidden-composing-cursor", "猫", -1, -1, "a猫🙂")
        hidden_again = transaction("ime-hidden-composing-cursor-repeat", "猫", -1, -1, "a猫🙂")
        restored = transaction("ime-visible-composing-cursor-restored", "猫", 3, 3, "a猫🙂")
        assert shown["caret"] == hidden["caret"] == hidden_again["caret"] == restored["caret"], (
            shown, hidden, hidden_again, restored)
        x, y, width, height = shown["caret"]
        assert x > finished["caret"][0] + finished["caret"][2], (finished, shown)
        # The private profile's line caret ends in blank space after the new glyph.
        # Its center strip has a real visible-caret control before AND after two
        # independent hidden-preedit protocol batches/owned-surface paints.
        caret_bounds = (x + width // 2, y + math.ceil(height * .2),
                        x + width // 2 + 1, y + math.floor(height * .7))
        background = pixels("ime-unicode-finished", caret_bounds)
        for label in ("ime-visible-composing-cursor", "ime-visible-composing-cursor-restored"):
            assert ImageChops.difference(background, pixels(label, caret_bounds)).getbbox(), (
                "Visible preedit cursor control did not paint its expected cell", label, caret_bounds)
        for label in ("ime-hidden-composing-cursor", "ime-hidden-composing-cursor-repeat"):
            assert ImageChops.difference(background, pixels(label, caret_bounds)).getbbox() is None, (
                "Hidden preedit cursor still paints its expected cell", label, caret_bounds)
        framed_keys(window, "ctrl+g")
        cancelled = observe("ime-real-c-g-cancel", "a猫🙂")
        assert cancelled["caret"] == finished["caret"], (
            "Real C-g did not restore the committed insertion rectangle", finished, cancelled)
        evidence = {"glyph_bounds": glyph_bounds, "cancel_bounds": cancel_bounds,
                    "caret_bounds": caret_bounds, "captures": {label: str(file) for label, file in captures.items()},
                    "blocked_clipboard": {"text": clipboard_payload, "before": preedit, "after": clipboard_preedit},
                    "committed_glyph_sha256": hashlib.sha256(glyphs.tobytes()).hexdigest()}
        evidence_path = destination.with_name(destination.stem + "-ime-visual-evidence.json")
        evidence_path.write_text(json.dumps(evidence, indent=2) + "\n")
        print("Native IME glyph, cancellation and hidden-cursor pixel proof:", evidence_path, flush=True)
    return "a猫🙂"


def pane_metrics(layout, zoom):
    """Locate controls from the allocated conversation and its native header click band."""
    values = layout["conversation"]
    x, y, width, height = values["rect"]
    short = height < 360 * zoom
    inset = 0
    left = (8 if short else 12) * zoom
    padding_y, panel_gap = (0 if short else 8 * zoom), (6 if short else 12) * zoom
    extent = max(32, 32 * zoom)
    card_width = width
    header_y = 0
    header_minimum = min(extent + (0 if short else 12 * zoom), max(0, height))
    top = header_minimum + panel_gap
    return {"x": x, "y": y, "left": left, "inset": inset, "padding_y": padding_y,
            "gap": panel_gap, "inner_width": max(0, width - 2 * left), "top": top,
            "available": max(0, height - inset - top), "card_width": card_width,
            "header_y": header_y, "header_minimum": header_minimum, "extent": extent}


def title_actions(layout, zoom, *, channel=True, narrow=False):
    metrics = pane_metrics(layout, zoom)
    extent, gap, width = metrics['extent'], 6 * zoom, metrics['card_width']
    names = (('people', 'topic', 'more') if narrow else
             ('people', 'topic', 'search', 'more')) if channel else (
                 ('more',) if narrow else ('search', 'more'))
    reserved = len(names) * extent + (len(names) - 1) * gap
    full_count = 4 if channel else 2
    full_reserved = full_count * extent + (full_count - 1) * gap
    padding = min(8 * zoom, max(0, (width - full_reserved) / 2))
    left = metrics['x'] + max(0, width - padding - reserved)
    return {name: (left + index * (extent + gap) + extent / 2,
                   metrics['y'] + metrics['header_minimum'] / 2)
            for index, name in enumerate(names)}


def open_title_menu(window, actions):
    return open_native_popup(window, lambda: click(window, *actions["more"]))


def composer_primary_targets(layout, zoom, caret):
    """Locate native Plus/Send without changing focus, selection, history or preedit."""
    assert caret is not None and caret[2] > 0 and caret[3] > 0, caret
    metrics = pane_metrics(layout, zoom)
    extent, gap = metrics["extent"], 4 * zoom
    right = metrics["x"] + layout["conversation"]["rect"][2] - metrics["left"]
    # These fixed-size controls are top-aligned with the native Input. A point
    # inside its first shaped row lies inside both; no text-width estimate or
    # document/cursor mutation is needed during a pending clipboard transaction.
    y = caret[1] + min(caret[3] / 2, extent / 4)
    result = {"plus": (right - 1.5 * extent - gap, y),
              "send": (right - extent / 2, y), "caret": caret, "extent": extent}
    for name in ("plus", "send"):
        x, y = result[name]
        assert metrics["x"] <= x < right and metrics["y"] + metrics["top"] <= y, (name, result, metrics)
        assert y < metrics["y"] + layout["conversation"]["rect"][3], (name, result, metrics)
    return result




def composer_targets(window, layout, zoom):
    """Locate the fixed primary row through actual first-line native caret geometry.

    These ordinary menu/Send consumers begin and end without a selection or
    active preedit. Native beginning/end commands preserve their end-of-document
    editing position; shape/trail/selection probes must not use this locator.
    """
    command("beginning-of-buffer", window=window)
    SESSION.until("native composer first-line caret", lambda: native_input_rectangle(window) is not None)
    caret = native_input_rectangle(window)
    command("end-of-buffer", window=window)
    result = composer_primary_targets(layout, zoom, caret)
    geometry = window_geometry(window)
    for name in ("plus", "send"):
        x, y = result[name]
        assert 0 <= x < geometry["WIDTH"] and 0 <= y < geometry["HEIGHT"], (name, result, geometry)
    return result


def await_native_frame(window, offset, trigger, events=1, *, surface=None):
    proc, path, root = GUI_DEBUG[window]
    surface = root if surface is None else int(surface)
    committed = re.compile(rf"wl_surface[@#]{surface}\.commit\(")
    attached = re.compile(rf"wl_surface[@#]{surface}\.attach\(wl_buffer[@#]\d+,")

    def observed():
        assert proc.poll() is None, f"GUI exited {proc.returncode} before native input frame"
        count = 0
        painted = False
        with path.open("rb") as log:
            log.seek(offset)
            records = log.read().decode("utf-8", errors="replace").splitlines()
        for line in records:
            if trigger.search(line):
                count += 1
            if count >= events and attached.search(line):
                painted = True
            if painted and committed.search(line):
                return True
        return False

    SESSION.until("real owned-surface buffer attachment/commit after native input", observed)


def native_input_state(window, *, offset=0, initial=None, record_bytes=None):
    """Read native state; explicit bytes or a prior snapshot can seed a finite observation."""
    assert offset == 0 or initial is not None
    state = dict(initial) if initial is not None else None
    # libwayland prints string payloads literally, including UTF-8, quotes,
    # control bytes and newlines. Split actual timestamped protocol records,
    # rather than treating surrounding text as one line or a JSON string.
    if record_bytes is None:
        with GUI_DEBUG[window][1].open("rb") as log:
            log.seek(offset)
            text = log.read().decode("utf-8", errors="replace")
    else:
        assert offset == 0
        text = record_bytes.decode("utf-8", errors="replace")
    records = re.split(r"(?m)(?=^\[[^\]\n]+\]\s+\{)", text)
    for line in records:
        if "zwp_text_input_v3" not in line:
            continue
        if re.search(r"\.enable\(\)", line):
            state = {"caret": None, "text": None, "cursor": None, "anchor": None}
        elif re.search(r"\.disable\(\)", line):
            state = None
        elif state is not None:
            rectangle = re.search(r"\.set_cursor_rectangle\((-?\d+),\s*(-?\d+),\s*(\d+),\s*(\d+)\)", line)
            surrounding = re.search(r'\.set_surrounding_text\("(.*)",\s*(\d+),\s*(\d+)\)\s*(?:\n|$)',
                                    line, re.DOTALL)
            if rectangle:
                state["caret"] = tuple(map(int, rectangle.groups()))
            if surrounding:
                state.update(text=surrounding.group(1), cursor=int(surrounding.group(2)),
                             anchor=int(surrounding.group(3)))
    return state


def native_input_rectangle(window):
    state = native_input_state(window)
    return state["caret"] if state is not None else None


def observe_native_input(window, destination, label, text, cursor, anchor, *,
                         expected=None, after_action=0):
    """Observe a complete current native transaction and its later owned root paint."""
    result = None
    completed = None
    proc, path, surface = GUI_DEBUG[window]
    transaction = re.compile(
        rb"(?m)^\[[^\]\n]+\]\s+\{[^}\n]+\}\s+->\s+zwp_text_input_v3[@#]\d+\.commit\(\)\r?\n")
    pending = re.compile(
        rb"(?m)^\[[^\]\n]+\]\s+\{[^}\n]+\}\s+->\s+zwp_text_input_v3[@#]\d+\.(?:enable|disable|set_[a-z_]+)\(")
    attached = re.compile(rf"wl_surface[@#]{surface}\.attach\(wl_buffer[@#]\d+,")
    committed = re.compile(rf"wl_surface[@#]{surface}\.commit\(")

    def snapshot():
        assert proc.poll() is None, f"GUI exited before native observation {label}"
        data = path.read_bytes()
        last = None
        for match in transaction.finditer(data):
            last = match
        if last is None or last.end() <= after_action or pending.search(data, last.end()):
            return None
        end = last.end()
        state = native_input_state(window, record_bytes=data[:end])
        if (state is None or state["caret"] is None
                or (state["text"], state["cursor"], state["anchor"]) != (text, cursor, anchor)
                or (expected is not None and state != expected)):
            return None
        tail = data[end:].decode("utf-8", errors="replace")
        paint = attached.search(tail)
        if paint is None or committed.search(tail, paint.end()) is None:
            return None
        return state, end

    def observed():
        nonlocal result, completed
        current = snapshot()
        if current is None:
            return False
        state, end = current
        x, y, width, height = state["caret"]
        geometry = window_geometry(window)
        assert width > 0 and height > 0 and 0 <= x and 0 <= y, state
        assert x + width <= geometry["WIDTH"] and y + height <= geometry["HEIGHT"], (state, geometry)
        capture_window(window, destination, label)
        latest = snapshot()
        if latest is None or latest[0] != state:
            return False
        result, completed = latest
        return True

    SESSION.until("complete current native transaction and subsequent owned-buffer paint", observed)
    print("Native text-input state:", json.dumps({
        "phase": label, **result, "after_action": after_action, "transaction_end": completed,
    }), flush=True)
    return result


def observe_prefix_caret(window, destination, reduced_motion):
    state = observe_native_input(window, destination, "caret-prefix-start", "", 0, 0)
    x, y, width, height = state["caret"]
    geometry = window_geometry(window)
    region = f'{geometry["X"] + x},{geometry["Y"] + y} {width}x{height}'
    header = f"P6\n{width} {height}\n255\n".encode()
    variants = set()
    changes = []
    last = None
    samples = 0
    start = time.monotonic()
    # This is the existing deliberate 2.5-second C-x survival pause, observed at
    # real compositor frame boundaries rather than supplemented with a sleep.
    while time.monotonic() - start < 2.5:
        image = subprocess.run(["grim", "-t", "ppm", "-g", region, "-s", str(1), "-"],
                               env=SESSION.env, capture_output=True, check=True, timeout=10).stdout
        assert image.startswith(header), (header, image[:len(header)])
        pixels = image[len(header):]
        assert len(pixels) == width * height * 3, (len(pixels), width, height)
        variants.add(pixels)
        if pixels != last:
            changes.append(round(time.monotonic() - start, 3))
            last = pixels
        samples += 1
    assert samples >= 2, samples
    if reduced_motion:
        assert len(variants) == 1, ("Reduced-motion native caret did not stay steady", changes)
    else:
        assert len(variants) > 1, ("Focused native caret never visibly blinked during C-x pause", changes)
    assert native_input_state(window) == state, "Caret probe changed native input state"
    capture_window(window, destination, "caret-prefix-after-pause")
    print("Native caret raster:", json.dumps({
        "rect": state["caret"], "samples": samples, "variants": len(variants),
        "change_seconds": changes, "reduced_motion": reduced_motion,
    }), flush=True)


def framed_keys(window, *keys):
    marker = GUI_DEBUG[window][1].stat().st_size
    presses = key(*keys, trace=True)
    final = presses[-1]
    # Match the final injected key code's final occurrence, not an earlier
    # modifier press. The producer trace also handles wtype's dynamic keymap.
    await_native_frame(window, marker,
                       re.compile(rf"wl_keyboard[@#]\d+\.key\(\d+,\s*\d+,\s*{final},\s*1\)"),
                       presses.count(final))




def format_native_control(window, index, destination, label):
    """Activate a real upward-toolbar control through the Plus keyboard entrypoint."""
    assert 0 <= index < 39, index
    before = native_input_state(window)
    assert before is not None and before["caret"] is not None, before
    framed_keys(window, "shift+Tab", "Return")
    SESSION.until("native formatting toolbar owns non-text focus", lambda: native_input_state(window) is None)
    capture_window(window, destination, "format-toolbar-" + label + "-open")
    if index:
        framed_keys(window, *(["Tab"] * index))
    capture_window(window, destination, "format-toolbar-" + label + "-control")
    framed_keys(window, "Return")
    SESSION.until("formatting action restores native Input focus", lambda: native_input_rectangle(window) is not None)
    expanded = native_input_state(window)
    framed_keys(window, "shift+Tab", "Return")
    SESSION.until("closing formatting toolbar restores Input focus", lambda: native_input_rectangle(window) is not None)
    closed = native_input_state(window)
    assert closed == expanded, ("Closing formatting controls changed native editing state or insertion bounds",
                                expanded, closed)
    capture_window(window, destination, "format-toolbar-" + label + "-closed")
    return {"before": before, "after": closed}


def probe_native_formatting(proc, window, preferences, database_path, args, destination, *,
                            query_id, query_target, own_nick, receive):
    """Exercise actual toggles/samples, colored native glyphs, and exact raw IRC delivery."""
    from PIL import Image

    before = save_state(proc, preferences, database_path)
    assert before[1] == (query_id,), before
    _composer_native(window, "", 0, 0)
    expected = []
    for index, (name, code) in enumerate((
            ("bold", "\x02"), ("italic", "\x1d"), ("underline", "\x1f"),
            ("strike", "\x1e"), ("monospace", "\x11"), ("reverse", "\x16"),
            ("reset", "\x0f"))):
        format_native_control(window, index, destination, name + "-on")
        type_text("Abc")
        raw = code + "Abc"
        length = len(raw.encode("utf-8"))
        observe_native_input(window, destination, "format-" + name + "-typed", raw, length, length)
        format_native_control(window, index, destination, name + "-off")
        payload = raw + code
        framed_keys(window, "Return")
        receive("PRIVMSG", [query_target, payload], own_nick)
        expected.append((query_id, own_nick, payload, 1, 0))
        wait_messages(proc, database_path, expected)
        _composer_native(window, "", 0, 0)
    foreground = format_native_control(window, 7 + 4, destination, "foreground-04")
    background = format_native_control(window, 23 + 2, destination, "background-02")
    assert background["after"]["caret"] == foreground["before"]["caret"], (
        "Hidden IRC color parameters occupied native display cells", foreground, background)
    type_text("Color")
    raw = "\x0304\x02\x02\x0f\x034,2\x02\x02Color"
    length = len(raw.encode("utf-8"))
    typed = observe_native_input(window, destination, "format-colors-typed", raw, length, length)
    first = foreground["before"]["caret"]
    last = typed["caret"]
    top = max(first[1], last[1])
    bottom = min(first[1] + first[3], last[1] + last[3])
    bounds = (first[0] + first[2] + 1, top + 2, last[0] - 2, bottom - 2)
    assert bounds[0] < bounds[2] and bounds[1] < bounds[3], (first, last, bounds)
    capture = destination.with_name(destination.stem + "-format-colors-typed.png")
    with Image.open(capture) as image:
        rgb = image.convert("RGB").crop(bounds).tobytes()
    foreground, background = (216, 72, 72), (36, 86, 166)
    def foreground_ink(offset):
        coverage = (rgb[offset] - background[0]) / (foreground[0] - background[0])
        return .3 <= coverage <= 1.01 and all(
            abs(rgb[offset + channel] - (background[channel] +
                coverage * (foreground[channel] - background[channel]))) <= 4
            for channel in (1, 2))
    # Accept real antialiased glyph coverage over the selected background,
    # including small fonts that have no fully opaque foreground texel.
    counts = {"foreground_04": sum(foreground_ink(offset) for offset in range(0, len(rgb), 3)),
              "background_02": sum(rgb[offset:offset + 3] == bytes(background)
                                   for offset in range(0, len(rgb), 3))}
    assert all(count >= 4 for count in counts.values()), (
        "Actual composer glyph/background pixels do not show the selected IRC samples", bounds, counts)
    format_native_control(window, 6, destination, "colors-reset")
    payload = raw + "\x0f"
    framed_keys(window, "Return")
    receive("PRIVMSG", [query_target, payload], own_nick)
    expected.append((query_id, own_nick, payload, 1, 0))
    wait_messages(proc, database_path, expected)
    _composer_native(window, "", 0, 0)
    expected.extend(probe_composer_context(
        proc, window, preferences, database_path, args, destination,
        query_id=query_id, query_target=query_target, own_nick=own_nick, receive=receive))
    after = save_state(proc, preferences, database_path)
    assert after[0] == before[0] and after[1] == before[1], (before, after)
    assert [row for row in after[2] if row[0] != query_id] == [
        row for row in before[2] if row[0] != query_id], "Formatting changed another buffer"
    evidence = {"raw_payloads": [row[2] for row in expected], "color_bounds": bounds,
                "color_pixels": counts, "capture": str(capture)}
    destination.with_name(destination.stem + "-formatting-evidence.json").write_text(
        json.dumps(evidence, indent=2) + "\n")
    return expected


def probe_composer_context(proc, window, preferences, database_path, args, destination, *,
                           query_id, query_target, own_nick, receive):
    """Real selected-input clipboard/format actions and a constrained pointer popup."""
    text = "Native context selection"
    type_text(text)
    framed_keys(window, "ctrl+Home", "ctrl+shift+End")
    selected = _composer_native(window, text, len(text), 0)
    evidence = []

    def open_context(label, *, edge=False):
        state = native_input_state(window)
        assert state and state["caret"], state
        if edge:
            layout = read_layout(preferences)
            controls = composer_primary_targets(layout, args.zoom, state["caret"])
            point = (controls["plus"][0] - 2 * controls["extent"], controls["plus"][1])
        else:
            caret = state["caret"]
            point = (caret[0] - 3 * args.zoom, caret[1] + caret[3] / 2)

        def press():
            move_pointer(window, *point)
            POINTER.button(True, code=0x111)
            POINTER.button(False, code=0x111)

        popup = open_native_popup(window, press)
        record = pointer_popup_bounds(window, popup, point)
        record["capture"] = str(capture_native_popup(window, popup, destination, label))
        evidence.append(record)
        if edge:
            assert "y" in record["repositioned_axes"], ("Bottom-edge menu was not repositioned", record)
        return popup

    def tail_action(popup, offset):
        # Enabled/disabled Undo and Redo vary with real history. The stable tail
        # is Cut, Copy, Paste, Select all, Formatting, Foreground, Background.
        popup_keys(window, popup, "End", *(["Up"] * offset), "Return")

    popup = open_context("composer-context-selection-retained")
    popup_keys(window, popup, "Escape")
    assert _composer_native(window, text, len(text), 0) == selected
    popup = open_context("composer-context-copy")
    tail_action(popup, 5)
    SESSION.until("context Copy owns the real clipboard", lambda: clipboard_text() == text)
    _composer_native(window, text, len(text), 0)
    popup = open_context("composer-context-cut")
    tail_action(popup, 6)
    _composer_native(window, "", 0, 0)
    SESSION.until("context Cut owns the real clipboard", lambda: clipboard_text() == text)
    popup = open_context("composer-context-paste", edge=True)
    tail_action(popup, 4)
    _composer_native(window, text, len(text), len(text))
    framed_keys(window, "ctrl+Home", "ctrl+shift+End")
    _composer_native(window, text, len(text), 0)
    popup = open_context("composer-context-format", edge=True)
    open_native_submenu(window, popup, 2, from_end=True)
    popup_keys(window, popup, "Home", "Return")
    payload = "\x02" + text + "\x02"
    _composer_native(window, payload, len(payload), len(payload))
    framed_keys(window, "Return")
    receive("PRIVMSG", [query_target, payload], own_nick)
    expected = [(query_id, own_nick, payload, 1, 0)]
    wait_messages(proc, database_path, expected)
    _composer_native(window, "", 0, 0)
    destination.with_name(destination.stem + "-composer-context.json").write_text(
        json.dumps({"popups": evidence, "exact_wire_payload": payload}, indent=2) + "\n")
    return expected


def probe_native_emoji(proc, window, preferences, database_path, args, destination, *,
                       query_id, query_target, own_nick, receive):
    """Prove real fuzzy completion, dismissal and token boundaries through raw IRC delivery."""
    before = save_state(proc, preferences, database_path)
    assert before[1] == (query_id,), before
    _composer_native(window, "", 0, 0)
    expected = []

    def deliver(payload):
        framed_keys(window, "Return")
        receive("PRIVMSG", [query_target, payload], own_nick)
        expected.append((query_id, own_nick, payload, 1, 0))
        wait_messages(proc, database_path, expected)
        _composer_native(window, "", 0, 0)

    format_native_control(window, 0, destination, "emoji-bold-prefix")
    type_text(":smle")
    observe_native_input(window, destination, "emoji-formatted-fuzzy-query", "\x02:smle", 6, 6)
    framed_keys(window, "Return")
    emoji = "\U0001f604"
    completed = "\x02" + emoji
    length = len(completed.encode("utf-8"))
    observe_native_input(window, destination, "emoji-formatted-completed", completed, length, length)
    format_native_control(window, 0, destination, "emoji-bold-suffix")
    deliver(completed + "\x02")

    type_text(":")
    observe_native_input(window, destination, "emoji-bare-colon", ":", 1, 1)
    type_text("smle")
    observe_native_input(window, destination, "emoji-dismiss-query", ":smle", 5, 5)
    framed_keys(window, "Escape")
    observe_native_input(window, destination, "emoji-dismissed-unchanged", ":smle", 5, 5)
    deliver(":smle")

    for label, text in (("url", "https://example.org/:smle"), ("time", "12:30")):
        type_text(text)
        length = len(text.encode("utf-8"))
        observe_native_input(window, destination, "emoji-" + label + "-literal", text, length, length)
        deliver(text)
    after = save_state(proc, preferences, database_path)
    assert after[0] == before[0] and after[1] == before[1], (before, after)
    assert [row for row in after[2] if row[0] != query_id] == [
        row for row in before[2] if row[0] != query_id], "Emoji completion changed another buffer"
    evidence = {"raw_payloads": [row[2] for row in expected], "fuzzy_result": emoji,
                "formatting_prefix_retained": completed}
    destination.with_name(destination.stem + "-emoji-evidence.json").write_text(
        json.dumps(evidence, indent=2, ensure_ascii=False) + "\n")
    return expected


def sidebar_controls(layout, zoom):
    values = layout["channels"]
    assert values["mode"] == "expanded", values
    x, y, width, height = values["rect"]
    short = height < 360 * zoom
    search_height = max(32, (36 if short else 40) * zoom)
    # Only the unchanged left input inset is needed. Native text measurement
    # decides selector width and tabs-versus-selector; do not duplicate it.
    return {"query": (x + 20*zoom, y + 8*zoom + search_height/2)}


def sidebar_filter_popup(window):
    """Activate the filter after the focused query; observe its native shape."""
    framed_keys(window, "Tab")
    marker = GUI_DEBUG[window][1].stat().st_size
    presses = key("Return", trace=True)
    proc, path, root = GUI_DEBUG[window]
    trigger = re.compile(rf"wl_keyboard[@#]\d+\.key\(\d+,\s*\d+,\s*{presses[-1]},\s*1\)")

    def activated():
        assert proc.poll() is None, "GUI exited before native sidebar filter activation"
        text = path.read_bytes()[marker:].decode(errors="replace")
        event = trigger.search(text)
        if event is None:
            return False
        text = text[event.end():]
        # Select creates its popup role synchronously during Validate, before
        # root paint. A normal button instead activates All and paints the root.
        if re.search(r"\.get_popup\(new id xdg_popup[@#]\d+,", text):
            return "selector"
        attached = re.search(rf"wl_surface[@#]{root}\.attach\(wl_buffer[@#]\d+,", text)
        if attached and re.search(rf"wl_surface[@#]{root}\.commit\(", text[attached.end():]):
            return "tabs"
        return False

    kind = SESSION.until("native filter popup or All-tab root paint", activated)
    return await_native_popup(window, marker) if kind == "selector" else None


def filter_sidebar(window, layout, zoom, query, choice):
    controls = sidebar_controls(layout, zoom)
    click(window, *controls["query"])
    key("ctrl+a", "BackSpace")
    if query:
        type_text(query)
    popup = sidebar_filter_popup(window)
    if popup is not None:
        popup_keys(window, popup, "Home", *(["Down"] * choice), "Return")
    elif choice:
        framed_keys(window, *(["Tab"] * choice), "Return")
    return popup is not None


def activate_sidebar_item(window, choice, *, selector, activation="Return", skip_pinned=False):
    remaining = 0 if selector else 3 - choice
    # An excluded current conversation is pinned ahead of the network header.
    # Skip it to reach a normal match or the no-match recovery action.
    stops = remaining + 2 + int(skip_pinned)
    framed_keys(window, *(["Tab"] * stops), activation)


def clear_draft():
    """Clear the whole focused input without depending on Emacs mark state."""
    key("ctrl+Home", "ctrl+shift+End", "BackSpace")


def switch_buffer(target, network="Local"):
    window = _open_native_palette("ctrl+x", "b")
    query = target + " " + network
    type_text(query)
    _palette_query(window, query)
    key("Return")






def select_server(proc, window, preferences, database_path, args, name):
    before = save_state(proc, preferences, database_path)
    rail = read_layout(preferences)["rail"]["rect"]
    index = ("Local", "Other").index(name)
    point = (rail[0] + rail[2] / 2, rail[1] + (6 + 82 * index + 38) * args.zoom)
    marker = GUI_DEBUG[window][1].stat().st_size
    click(window, *point)
    await_native_frame(window, marker,
                       re.compile(r"wl_pointer[@#]\d+\.button\(\d+,\s*\d+,\s*272,\s*0\)"))
    selected = save_state(proc, preferences, database_path)
    assert selected[0]["selected_server"] == name, selected
    assert selected[1][0].startswith(name + "/"), selected
    assert selected[0]["native_navigation_mode"] == before[0]["native_navigation_mode"], (
        "Selecting even the already-selected server must not toggle channels", before, selected)
    return selected


def set_message_style(proc, window, preferences, database_path, args, destination,
                      buffer, mode, *, label):
    before = save_state(proc, preferences, database_path)
    assert before[1] == (buffer,), before
    settings = tomllib.loads(preferences.read_text())
    overrides = set(settings["presentation"]["irc_channels"])
    current = "irc" if buffer in overrides else "bubbles"
    if current != mode:
        channel = database_buffer_kind(database_path, buffer) == "channel"
        layout = read_layout(preferences)
        actions = title_actions(layout, args.zoom, channel=channel)
        popup = open_title_menu(window, actions)
        # Message display precedes the stable Presence/History/Details/Close tail.
        open_native_submenu(window, popup, 4, from_end=True)
        popup_keys(window, popup, "Home", *(["Down"] * (mode == "irc")), "Return")
        overrides.add(buffer) if mode == "irc" else overrides.discard(buffer)
        wait_preference(proc, preferences, "presentation", "irc_channels", sorted(overrides))
    after = save_state(proc, preferences, database_path)
    assert_same_state("Changing message display preserves conversations/drafts/read state", before, after)
    capture_window(window, destination, label)
    return sorted(overrides)


def database_buffer_kind(path, buffer):
    # The public global feed is deliberately transient, not a durable source
    # buffer. Other missing IDs must still fail the exact database assertion.
    if buffer == "!global-mentions":
        return "mentions"
    database = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    try:
        row = database.execute("SELECT kind FROM buffers WHERE id=?", (buffer,)).fetchone()
        assert row is not None, buffer
        return row[0]
    finally:
        database.close()


def semantic_workspace(workspace):
    fields = {"visible_buffers", "people_mode", "selected_server", "native_navigation_mode"}
    return {key: value for key, value in workspace.items()
            if key in fields or key.startswith("last_buffer:")}


def save_preferences(proc, preferences, window):
    """Use real Settings Save at stable boundaries, never a style/hidden command."""
    if SETTINGS_OPEN.get(window, False):
        # Settings callers use save_settings_footer for actual writes; a
        # snapshot must not move their active field or category focus.
        return tomllib.loads(preferences.read_text())
    open_settings(window, preferences)
    framed_keys(window, "shift+Tab")
    saved = save_settings_footer(window, saved_path=preferences)
    close_settings_navigation(window)
    # A retained text-input transaction can belong to a retired palette field.
    # Source-bound caller probes assert their actual composer document/selection;
    # this generic persistence helper cannot identify that source from the log.
    return saved


def close_settings_navigation(window):
    """Dismiss Settings from native category navigation using its cancel command."""
    framed_keys(window, "ctrl+g")


def save_state(proc, preferences, database_path):
    window = next((identifier for identifier, trace in GUI_DEBUG.items() if trace[0] is proc), None)
    assert window is not None, f"No owned native window for GUI process {proc.pid}"
    settings = save_preferences(proc, preferences, window)
    database = sqlite3.connect(f"file:{database_path}?mode=ro", uri=True)
    try:
        buffers = database.execute(
            "SELECT id,unread,read_marker,draft FROM buffers ORDER BY id").fetchall()
        active = database.execute("SELECT value FROM meta WHERE key='active_buffer'").fetchone()
        return semantic_workspace(settings["workspace"]), active, buffers
    finally:
        database.close()


def save_filter_baseline(proc, preferences, database_path, buffer_id, label):
    acknowledged = wait_read_marker(proc, database_path, buffer_id)
    baseline = save_state(proc, preferences, database_path)
    active_row = next(row for row in baseline[2] if row[0] == buffer_id)
    print("Filter baseline:", json.dumps({
        "label": label, "buffer": buffer_id,
        "newest_message": {"id": acknowledged[2], "text": acknowledged[3],
                           "server_time": acknowledged[4]},
        "acknowledged_marker": acknowledged[0],
        "snapshot_marker": active_row[2], "snapshot_unread": active_row[1],
    }, ensure_ascii=False), flush=True)
    return baseline


def assert_same_state(label, expected, actual):
    if actual != expected:
        fields = ("workspace", "active_buffer", "buffers (id, unread, read_marker, draft)")
        detail = {"expected": dict(zip(fields, expected)), "actual": dict(zip(fields, actual))}
        raise AssertionError(label + " changed native state:\n" +
                             json.dumps(detail, ensure_ascii=False, indent=2))


def assert_workspace(settings, active_buffer, people_mode, server="Local"):
    workspace = settings["workspace"]
    assert workspace["visible_buffers"] == active_buffer, workspace
    assert workspace["people_mode"] == people_mode, workspace
    assert workspace["selected_server"] == server, workspace
    assert workspace["last_buffer:" + server] == active_buffer, workspace


def assert_saved_buffers(path, expected, active):
    database = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    try:
        identities = tuple(row[0] for row in expected)
        buffers = database.execute(
            "SELECT id,server,target,kind,draft FROM buffers WHERE id IN (" +
            ",".join("?" for _ in identities) + ") ORDER BY id", identities,
        ).fetchall()
        assert buffers == sorted(expected), buffers
        saved_active = database.execute("SELECT value FROM meta WHERE key='active_buffer'").fetchone()
        assert saved_active == (active,), saved_active
    finally:
        database.close()


def collect_gui(proc, timeout=30):
    stdout, _ = proc.communicate(timeout=timeout)
    proc.native_debug_stream.close()
    return stdout, proc.native_debug_path.read_text(errors="replace")


def launch_gui(binary, screenshot, env, size, logs):
    debug_path = screenshot.with_suffix(".wayland.log")
    debug_stream = debug_path.open("wb")
    try:
        proc = subprocess.Popen([str(binary), "--size", f"{size[0]}x{size[1]}",
                                 "--screenshot", str(screenshot)],
                                cwd=ROOT, env=env | {"WAYLAND_DEBUG": "client"},
                                stdout=subprocess.PIPE, stderr=debug_stream, text=True,
                                start_new_session=True)
    except BaseException:
        debug_stream.close()
        raise
    proc.native_debug_path, proc.native_debug_stream = debug_path, debug_stream
    proc.native_database_path = Path(env["XDG_DATA_HOME"]) / "blastirc/state.sqlite3"
    try:
        def owned_window():
            if proc.poll() is not None:
                raise AssertionError(f"GUI exited {proc.returncode} while mapping its native window")
            windows = [node for node, _ in tree_windows(SESSION.ipc("get_tree"))]
            assert all(node["pid"] == proc.pid for node in windows), windows
            assert len(windows) <= 1, windows
            return windows[0] if windows else None

        native = SESSION.until("one owned native window", owned_window)
        assert native["shell"] == "xdg_shell", native
        assert native["app_id"] == "io.github.trevarj.blastirc", native
        window = native["id"]
        surface, _, _ = main_surface_ids(debug_path)
        GUI_DEBUG[window] = (proc, debug_path, surface)
        SESSION.ipc("command", f"[con_id={window}] border none, focus")
        SESSION.until("committed client dimensions",
                      lambda: (window_geometry(window)["WIDTH"], window_geometry(window)["HEIGHT"]) == size)
        geometry = window_geometry(window)
        screen = display_geometry()
        print(f"Wayland input environment display={SESSION.env['WAYLAND_DISPLAY']} "
              f"output={screen[0]}x{screen[1]} window={window} geometry={geometry}", flush=True)
        assert (geometry["WIDTH"], geometry["HEIGHT"]) == size, (size, geometry)
        assert (0 <= geometry["X"] and 0 <= geometry["Y"] and
                geometry["X"] + geometry["WIDTH"] <= screen[0] and
                geometry["Y"] + geometry["HEIGHT"] <= screen[1]), (
                    f"Native input window is offscreen: output={screen}, window={geometry}")
        return proc, window
    except BaseException:
        stop_group(proc)
        stdout, stderr = collect_gui(proc)
        logs.append(("launch failure", stdout, stderr))
        raise


def finish_gui(proc, window, screenshot, destination, size):
    geometry = window_geometry(window)
    expected = geometry["WIDTH"], geometry["HEIGHT"]
    assert expected == size, (expected, size)
    command("quit")
    stdout, stderr = collect_gui(proc)
    if proc.returncode:
        raise AssertionError(f"GUI exited {proc.returncode}\n{stdout}\n{stderr}")
    image = screenshot.read_bytes()
    assert image[:2] == b"BM", "Expected a BMP screenshot"
    width, height = struct.unpack_from("<ii", image, 18)
    assert (width, abs(height)) == expected, ((width, height), expected)
    destination.write_bytes(image)
    print("Normal-quit screenshot:", destination, flush=True)
    return stdout, stderr


def await_native_popup(window, offset, parent=None):
    path = GUI_DEBUG[window][1]
    root_surface = GUI_DEBUG[window][2]
    trace = path.read_text(errors="replace")
    root = re.search(rf"get_xdg_surface\(new id xdg_surface[@#](\d+),\s*wl_surface[@#]{root_surface}\)", trace)
    assert root is not None, "Root xdg_surface mapping missing from actual Wayland trace"
    root_xdg = root.group(1)
    parent_xdg = root_xdg if parent is None else parent["xdg"]
    result = None

    def mapped():
        nonlocal result
        with path.open("rb") as log:
            log.seek(offset)
            text = log.read().decode(errors="surrogateescape")
        for surface in re.finditer(r"get_xdg_surface\(new id xdg_surface[@#](\d+),\s*wl_surface[@#](\d+)\)", text):
            xdg, native = surface.groups()
            tail = text[surface.end():]
            retired = re.search(rf"xdg_surface[@#]{xdg}\.destroy\(", tail)
            if retired is not None:
                tail = tail[:retired.start()]
            popup = re.search(rf"xdg_surface[@#]{xdg}\.get_popup\(new id xdg_popup[@#](\d+),\s*xdg_surface[@#]{parent_xdg},", tail)
            if popup is None:
                continue
            role = popup.group(1)
            creation = surface.end() + popup.start()
            if re.search(rf"xdg_popup[@#]{role}\.destroy\(", text[creation:]):
                continue
            generation = text[creation:]
            configured = re.search(rf"xdg_popup[@#]{role}\.configure\((-?\d+),\s*(-?\d+),\s*(\d+),\s*(\d+)\)", generation)
            serial = re.search(rf"xdg_surface[@#]{xdg}\.configure\((\d+)\)", generation)
            if configured is None or serial is None:
                continue
            ack = re.search(rf"xdg_surface[@#]{xdg}\.ack_configure\({serial.group(1)}\)", generation[serial.end():])
            if ack is None:
                continue
            after_ack = generation[serial.end() + ack.end():]
            attached = re.search(rf"wl_surface[@#]{native}\.attach\(wl_buffer[@#]\d+,", after_ack)
            if attached is None or not re.search(rf"wl_surface[@#]{native}\.commit\(", after_ack[attached.end():]):
                continue
            rectangle = tuple(map(int, configured.groups()))
            geometry = window_geometry(window)
            x, y, width, height = rectangle
            windows = [(node, output_name) for node, output_name in tree_windows(SESSION.ipc("get_tree"))
                       if node["id"] == window]
            assert len(windows) == 1, windows
            outputs = [item for item in SESSION.ipc("get_outputs")
                       if item["active"] and item["name"] == windows[0][1]]
            assert len(outputs) == 1, outputs
            output = outputs[0]
            bounds = output["rect"]
            parent_x, parent_y = (geometry["X"], geometry["Y"]) if parent is None else parent["global_rect"][:2]
            global_x, global_y = parent_x + x, parent_y + y
            assert width > 0 and height > 0, rectangle
            # xdg_popup coordinates are parent-local; compositor constraints are
            # output-global. A centered small parent may legally have x=-5.
            assert bounds["x"] <= global_x and bounds["y"] <= global_y, (rectangle, geometry, bounds)
            assert global_x + width <= bounds["x"] + bounds["width"], (rectangle, geometry, bounds)
            assert global_y + height <= bounds["y"] + bounds["height"], (rectangle, geometry, bounds)
            result = {"surface": native, "xdg": xdg, "popup": role,
                      "rect": (global_x - geometry["X"], global_y - geometry["Y"], width, height),
                      "parent_rect": rectangle, "parent_xdg": parent_xdg,
                      "global_rect": (global_x, global_y, width, height),
                      "output_rect": bounds, "output_scale": output["scale"],
                      "creation_offset": offset + len(text[:creation].encode("utf-8", errors="surrogateescape"))}
            return True
        return False

    SESSION.until("configured/acknowledged native popup with its own attached buffer commit", mapped)
    print("Native popup protocol:", json.dumps(result), flush=True)
    return result


def capture_native_popup(window, popup, destination, label):
    """Capture the whole real popup, including legal portions outside its parent."""
    assert GUI_DEBUG[window][0].poll() is None
    x, y, width, height = popup["global_rect"]
    scale = popup["output_scale"]
    path = destination.with_name(destination.stem + "-" + label + "-popup.png")
    pixels = tuple(round(value * scale) for value in (width, height))
    capture_png(SESSION.env, path, scale=scale, expected_pixels=pixels,
                region=f"{x},{y} {width}x{height}")
    print("Native whole-popup screenshot:", path, flush=True)
    return path


def open_native_popup(window, action):
    marker = GUI_DEBUG[window][1].stat().st_size
    action()
    return await_native_popup(window, marker)


def pointer_popup_bounds(window, popup, point):
    """Check real content/shadow bounds against this press, not the root viewport."""
    trace = GUI_DEBUG[window][1].read_text(errors="replace")
    request = list(re.finditer(
        rf"xdg_surface[@#]{popup['xdg']}\.get_popup\(new id xdg_popup[@#]{popup['popup']},"
        r"\s*xdg_surface[@#]\d+,\s*xdg_positioner[@#](\d+)\)", trace))
    assert request, ("No real pointer popup positioner", popup)
    positioner = request[-1][1]
    anchors = list(re.finditer(
        rf"xdg_positioner[@#]{positioner}\.set_anchor_rect\((-?\d+),\s*(-?\d+),\s*(\d+),\s*(\d+)\)",
        trace[:request[-1].start()]))
    assert anchors, ("No real popup anchor rectangle", popup)
    anchor = tuple(map(int, anchors[-1].groups()))
    # Native popup chrome may expand its wire anchor to compensate shadows on
    # flip. Record that implementation detail, but measure against this press.
    regions, card = {}, None
    for line in trace[request[-1].end():].splitlines():
        match = re.search(r"create_region\(new id wl_region[@#](\d+)\)", line)
        if match:
            regions[match[1]] = []
        match = re.search(r"wl_region[@#](\d+)\.add\((-?\d+),\s*(-?\d+),\s*(\d+),\s*(\d+)\)", line)
        if match and match[1] in regions:
            regions[match[1]].append(tuple(map(int, match.groups()[1:])))
        match = re.search(rf"wl_surface[@#]{popup['surface']}\.set_input_region\(wl_region[@#](\d+)\)", line)
        if match:
            assert len(regions.get(match[1], [])) == 1, (line, regions)
            card = regions[match[1]][0]
    assert card is not None, "Pointer menu did not publish real native content bounds"
    sx, sy, sw, sh = popup["global_rect"]
    cx, cy, cw, ch = card
    assert 0 <= cx < cx + cw <= sw and 0 <= cy < cy + ch <= sh, (card, popup)
    geometry = window_geometry(window)
    cursor = (geometry["X"] + math.floor(point[0]), geometry["Y"] + math.floor(point[1]))
    content = (sx + cx, sy + cy, cw, ch)
    output = popup["output_rect"]
    repositioned = []
    for axis, origin, size, inset, surface_size, low, extent in (
            ("x", content[0], cw, cx, sw, output["x"], output["width"]),
            ("y", content[1], ch, cy, sh, output["y"], output["height"])):
        coordinate = cursor[0 if axis == "x" else 1]
        desired_surface = coordinate - inset
        if low <= desired_surface and desired_surface + surface_size <= low + extent:
            assert abs(origin - coordinate) <= 2, (axis, content, cursor, card, popup)
        else:
            distance = max(origin - coordinate, coordinate - (origin + size), 0)
            assert distance <= 2, (axis, content, cursor, card, popup)
            assert abs(origin - coordinate) > 2, ("Popup failed edge repositioning", axis, popup)
            repositioned.append(axis)
    result = {"trigger": point, "anchor": anchor, "content_global": content,
              "surface_global": popup["global_rect"], "content_in_surface": card,
              "shadow_insets": (cx, cy, sw - cx - cw, sh - cy - ch),
              "repositioned_axes": repositioned}
    print("Native pointer popup bounds:", json.dumps(result), flush=True)
    return result


def open_native_submenu(window, parent, index, *, from_end=False):
    """Open a branch by its offset among this level's selectable entries."""
    path = GUI_DEBUG[window][1]

    def child_of(popup):
        # Replay this still-live parent's generation, not every historical use
        # of its reusable Wayland object IDs.
        start = popup["creation_offset"]
        data = path.read_bytes()[start:]
        surfaces, children = {}, {}
        events = (rb"get_xdg_surface\(new id xdg_surface[@#](\d+),\s*wl_surface[@#]\d+\)"
                  rb"|xdg_surface[@#](\d+)\.get_popup\(new id xdg_popup[@#](\d+),\s*xdg_surface[@#](\d+),"
                  rb"|xdg_popup[@#](\d+)\.destroy\(")
        for event in re.finditer(events, data):
            surface, xdg, role, owner, destroyed = event.groups()
            if surface:
                surfaces[surface] = start + event.start()
            elif role and owner.decode() == popup["xdg"]:
                children[role] = (surfaces[xdg], start + event.start())
            elif destroyed:
                assert destroyed.decode() != popup["popup"], "Native submenu parent was destroyed"
                children.pop(destroyed, None)
        assert len(children) <= 1, ("Ambiguous live native submenu", children)
        if children:
            surface_offset, creation_offset = next(iter(children.values()))
            child = await_native_popup(window, surface_offset, parent=popup)
            assert child["creation_offset"] == creation_offset, "Native submenu generation changed"
            return child

    def family():
        members = [parent]
        while (child := child_of(members[-1])) is not None:
            members.append(child)
        return members

    members = family()
    geometry = window_geometry(window)
    width, height = geometry["WIDTH"], geometry["HEIGHT"]
    corners = (((0, 0), (1, 0)), ((width-1, 0), (width-2, 0)),
               ((0, height-1), (1, height-1)), ((width-1, height-1), (width-2, height-1)))
    pair = next((pair for pair in corners if all(
        not (item["rect"][0] <= point[0] < item["rect"][0] + item["rect"][2]
             and item["rect"][1] <= point[1] < item["rect"][1] + item["rect"][3])
        for point in pair for item in members)), None)
    assert pair is not None, "No root edge outside the observed native popup family"
    parked_after = path.stat().st_size
    # Two distinct safe positions guarantee delivery even if already parked at
    # the first. Observe the GUI connection, not just the virtual input client.
    move_pointer(window, *pair[0])
    move_pointer(window, *pair[1])
    root = GUI_DEBUG[window][2]
    px, py = pair[1]
    parked = re.compile(
        rf"wl_pointer[@#]\d+\.(?:motion\(\d+,|enter\(\d+,\s*wl_surface[@#]{root},)"
        rf"\s*{px}(?:\.0+)?,\s*{py}(?:\.0+)?\)")
    SESSION.until("native pointer delivered outside the popup family", lambda:
                  parked.search(path.read_bytes()[parked_after:].decode(errors="replace")))
    while len(members := family()) > 1:
        child, ancestor = members[-1], members[-2]
        closed_after = path.stat().st_size
        key("Left")

        def closed():
            data = path.read_bytes()
            for retained in (parent, ancestor):
                assert not re.search(
                    rf"xdg_popup[@#]{retained['popup']}\.destroy\(".encode(),
                    data[retained["creation_offset"]:]), "Left destroyed a native submenu parent"
            return re.search(rf"xdg_popup[@#]{child['popup']}\.destroy\(".encode(),
                             data[closed_after:]) is not None

        SESSION.until("Left destroys only the observed deepest native submenu", closed)
    marker = GUI_DEBUG[window][1].stat().st_size
    key("End" if from_end else "Home", *(["Up" if from_end else "Down"] * index), "Right")
    return await_native_popup(window, marker, parent=parent)


def select_native_menu(window, popup, path, *, root_changed=True):
    current = popup
    for index in path[:-1]:
        current = open_native_submenu(window, current, index)
    # Selection closes the entire family; wait for its root, not just the leaf.
    popup_keys(window, popup, "Home", *(["Down"] * path[-1]), "Return",
               root_changed=root_changed)


def popup_keys(window, popup, *keys, root_changed=True):
    marker = GUI_DEBUG[window][1].stat().st_size
    key(*keys)

    def destroyed():
        with GUI_DEBUG[window][1].open("rb") as log:
            log.seek(marker)
            text = log.read().decode(errors="replace")
        return re.search(rf"xdg_popup[@#]{popup['popup']}\.destroy\(", text) is not None

    SESSION.until("the activated or cancelled native popup role destroyed", destroyed)
    if root_changed:
        await_native_frame(window, marker,
                           re.compile(r"wl_keyboard[@#]\d+\.key\(\d+,\s*\d+,\s*\d+,\s*1\)"), len(keys))


def wait_preference(proc, path, section, name, value):
    def saved():
        if proc.poll() is not None:
            raise AssertionError(f"GUI exited {proc.returncode} while saving {section}.{name}")
        try:
            return tomllib.loads(path.read_text()).get(section, {}).get(name) == value
        except (FileNotFoundError, tomllib.TOMLDecodeError):
            return False
    SESSION.until(f"real Settings Save persisted {section}.{name}={value!r}", saved)


def settings_category(window, zoom, index, destination=None):
    """Select a category with an empty query, from category or Search focus.

    Compact callers land on Search so their next Tab reaches the first detail
    control, as it does from wide category navigation.
    """
    assert 0 <= index < 7, index
    bounds = window_geometry(window)
    compact = (min(1000 * zoom, bounds["WIDTH"] - 24) < 760 * zoom or
               min(720 * zoom, bounds["HEIGHT"] - 24) < 440 * zoom)
    search = native_input_state(window)
    if search is not None:
        assert search["text"] == "", "Count-based category helpers require an empty Settings query"
        framed_keys(window, "shift+Tab" if compact else "Tab")
    if compact:
        popup = open_native_popup(window, lambda: key("Return"))
        if destination is not None:
            capture_window(window, destination, f"settings-compact-category-selector-{index}")
            capture_native_popup(window, popup, destination, f"settings-compact-category-selector-{index}")
        popup_keys(window, popup, "Home", *(["Down"] * index), "Return", root_changed=False)
        framed_keys(window, "Tab")
    else:
        framed_keys(window, "Home", *(["Down"] * index))


def settings_select(window, index):
    """Activate the focused real Select, including its own popup-buffer barrier."""
    popup = open_native_popup(window, lambda: key("Return"))
    popup_keys(window, popup, "Home", *(["Down"] * index), "Return", root_changed=False)
    # A profile selection rebuilds and focuses its first field; other selections
    # retain their control. This benign round trip paints either actual result.
    framed_keys(window, "Tab", "shift+Tab")


def settings_field(window, value, destination, label):
    framed_keys(window, "ctrl+a", "BackSpace")
    if value:
        type_text(value)
    framed_keys(window, "End")
    end = len(value.encode("utf-8"))
    return observe_native_input(window, destination, label, value, end, end)


def save_settings_footer(window, *, saved_path, destination=None, label="settings-save"):
    """Save from empty-query category navigation, or Search with any query."""
    before = saved_path.stat()
    preferences = Path(SESSION.env["XDG_CONFIG_HOME"]) / "blastirc/settings.toml"
    scale = tomllib.loads(preferences.read_text())["appearance"]["scale"]
    bounds = window_geometry(window)
    compact = (min(1000 * scale, bounds["WIDTH"] - 24) < 760 * scale or
               min(720 * scale, bounds["HEIGHT"] - 24) < 440 * scale)
    # Category callers keep an empty query. From Search, Save/Close precede
    # the field even when a nonempty query exposes Matching settings after it.
    search = native_input_state(window)
    footer_stops = (2 if (compact and search is None) or
                    (not compact and search is not None) else 3)
    framed_keys(window, *(["shift+Tab"] * footer_stops))
    framed_keys(window, "Return")

    def written():
        if GUI_DEBUG[window][0].poll() is not None:
            raise AssertionError("GUI exited while saving the native Settings form")
        try:
            actual = saved_path.stat()
            if (actual.st_ino, actual.st_mtime_ns) == (before.st_ino, before.st_mtime_ns):
                return False
            return tomllib.loads(saved_path.read_text())
        except (FileNotFoundError, tomllib.TOMLDecodeError):
            return False

    saved = SESSION.until("native Save now completed a fresh private-file write", written)
    # This later native input/root paint follows the synchronous Core disk write,
    # rather than treating the queued request or old global status as success.
    framed_keys(window, *(["Tab"] * footer_stops))
    if destination is not None:
        capture_window(window, destination, label)
    return saved




def set_native_cursor_preferences(proc, window, prefs, db, args, dest, *,
                                  shape, effect, duration=120, strength=.2):
    assert shape in ("line", "box", "underline") and effect in ("off", "trail", "comet", "ribbon", "pulse")
    assert math.isfinite(duration) and 40 <= duration <= 240 and int(duration) == duration
    assert math.isfinite(strength) and .05 <= strength <= .35
    steps = round((strength - .05) / .01)
    assert math.isclose(.05 + steps * .01, strength, abs_tol=1e-6), strength
    baseline = tomllib.loads(prefs.read_text())
    open_settings(window, prefs)
    framed_keys(window, "shift+Tab")
    settings_category(window, args.zoom, 0)
    shape_stop = 12
    framed_keys(window, *(["Tab"] * shape_stop))
    settings_select(window, ("line", "box", "underline").index(shape))
    label = f"settings-cursor-{shape}-{effect}-{duration:g}-{strength:g}"
    capture_window(window, dest, label + "-shape")
    framed_keys(window, "Tab")
    settings_select(window, ("off", "trail", "comet", "ribbon", "pulse").index(effect))
    capture_window(window, dest, label + "-effective-policy")
    pages, remainder = divmod(int(duration) - 40, 10)
    framed_keys(window, "Tab", "Home", *(["Page_Up"] * pages), *(["Right"] * remainder))
    capture_window(window, dest, label + "-duration")
    framed_keys(window, "Tab", "Home", *(["Right"] * steps))
    capture_window(window, dest, label + "-strength")
    framed_keys(window, *(["shift+Tab"] * (shape_stop + 3)))
    saved = save_settings_footer(window, saved_path=prefs, destination=dest, label=label + "-saved")
    cursor = saved["cursor"]
    assert cursor["shape"] == shape and cursor["effect"] == effect, cursor
    assert math.isclose(cursor["trail_duration_ms"], duration, abs_tol=1e-5), cursor
    assert math.isclose(cursor["trail_strength"], strength, abs_tol=1e-6), cursor
    for section in ("appearance", "audio"):
        assert saved[section] == baseline[section], f"Cursor controls changed {section} suppression preferences"
    framed_keys(window, *(["Tab"] * (shape_stop + 4)), "Return")  # Try in conversation.
    SETTINGS_OPEN[window] = False
    save_state(proc, prefs, db)
    wait_layout(proc, prefs, predicate=lambda layout: "settings" not in layout)
    capture_window(window, dest, label + "-conversation")
    actual = tomllib.loads(prefs.read_text())
    assert actual["cursor"] == cursor, (cursor, actual["cursor"])
    return actual


def native_capture_timestamps(protocol):
    """Read completed screencopy timestamps, retaining EXT frame-object association."""
    pending_ext = {}
    completed = []
    events = re.finditer(
        rb'(?<!\w)(zwlr_screencopy_frame_v1|ext_image_copy_capture_frame_v1)[@#](\d+)'
        rb'\.(ready|presentation_time)\(([^()\r\n]*)\)', protocol)
    for event in events:
        interface, frame, name, arguments = event.groups()
        if interface == b'ext_image_copy_capture_frame_v1' and name == b'ready':
            timestamp = pending_ext.pop(frame, None)
            if not arguments.strip() and timestamp is not None:
                completed.append({'protocol': interface.decode(), 'frame': int(frame),
                                  'timestamp_ns': timestamp})
            continue
        if ((interface == b'zwlr_screencopy_frame_v1' and name != b'ready') or
                (interface == b'ext_image_copy_capture_frame_v1' and name != b'presentation_time')):
            continue
        fields = re.fullmatch(rb'\s*(\d+),\s*(\d+),\s*(\d+)\s*', arguments)
        if interface == b'ext_image_copy_capture_frame_v1':
            pending_ext.pop(frame, None)
        if fields is None:
            continue
        high, low, nanoseconds = map(int, fields.groups())
        if high >= 2**32 or low >= 2**32 or nanoseconds >= 1_000_000_000:
            continue
        timestamp = ((high << 32) | low) * 1_000_000_000 + nanoseconds
        if interface == b'ext_image_copy_capture_frame_v1':
            pending_ext[frame] = timestamp
        else:
            completed.append({'protocol': interface.decode(), 'frame': int(frame),
                              'timestamp_ns': timestamp})
    return completed


def check_native_capture_timestamps():
    """Runnable parser self-check; no compositor, subprocess or private model."""
    assert native_capture_timestamps(
        b'zwlr_screencopy_frame_v1#7.ready(1, 2, 3)') == [
            {'protocol': 'zwlr_screencopy_frame_v1', 'frame': 7,
             'timestamp_ns': ((1 << 32) | 2) * 1_000_000_000 + 3}]
    presentation = b'ext_image_copy_capture_frame_v1@7.presentation_time(0, 2, 3)\n'
    ready = b'ext_image_copy_capture_frame_v1@7.ready()\n'
    assert native_capture_timestamps(presentation + ready + ready) == [
        {'protocol': 'ext_image_copy_capture_frame_v1', 'frame': 7,
         'timestamp_ns': 2_000_000_003}]
    assert native_capture_timestamps(presentation) == []
    assert native_capture_timestamps(ready + presentation) == []
    assert native_capture_timestamps(
        presentation + b'ext_image_copy_capture_frame_v1@7.ready(0)') == []
    assert native_capture_timestamps(
        presentation + b'ext_image_copy_capture_frame_v1@8.ready()') == []
    assert native_capture_timestamps(
        presentation + b'ext_image_copy_capture_frame_v1@7.presentation_time(0, 2, 1000000000)\n' + ready) == []
    for malformed in (b'', b'0, 2', b'-1, 2, 3', b'4294967296, 2, 3', b'0, 2, 1000000000'):
        assert native_capture_timestamps(b'zwlr_screencopy_frame_v1#7.ready(' + malformed + b')') == []
    assert native_capture_timestamps(b'other_frame#7.ready(0, 2, 3)') == []
    assert native_capture_timestamps(b'not_zwlr_screencopy_frame_v1#7.ready(0, 2, 3)') == []


def native_rgb_capture(window, *, geometry=None, capture_timing=False):
    """Read real pixels; finite samplers may supply geometry and verify it after their batch."""
    check_geometry = geometry is None
    if check_geometry:
        geometry = window_geometry(window)
    region = f'{geometry["X"]},{geometry["Y"]} {geometry["WIDTH"]}x{geometry["HEIGHT"]}'
    capture_env = SESSION.env | {"WAYLAND_DEBUG": "client"} if capture_timing else SESSION.env
    captured = subprocess.run(["grim", "-t", "ppm", "-g", region, "-s", str(1), "-"],
                              env=capture_env, capture_output=True, check=True, timeout=10)
    image = captured.stdout
    header = re.match(rb"P6\n(\d+) (\d+)\n255\n", image)
    assert header is not None, ("Unexpected actual grim PPM header", image[:64])
    width, height = map(int, header.groups())
    assert (width, height) == (geometry["WIDTH"], geometry["HEIGHT"]), (width, height, geometry)
    pixels = image[header.end():]
    assert len(pixels) == width * height * 3, (len(pixels), width, height)
    if check_geometry:
        assert window_geometry(window) == geometry, "Client moved during the actual RGB capture"
    result = {"geometry": geometry, "width": width, "height": height, "pixels": pixels}
    if capture_timing:
        result.update(capture_timestamps=native_capture_timestamps(captured.stderr),
                      capture_protocol=captured.stderr)
    return result


def native_marker_components(image, rectangle, rgb):
    """Find complete exact-RGB components strictly inside the actual saved pane bounds."""
    x, y, width, height = rectangle
    left, top = max(0, math.ceil(x)), max(0, math.ceil(y))
    right = min(image["width"], math.floor(x + width))
    bottom = min(image["height"], math.floor(y + height))
    assert left < right and top < bottom, (rectangle, image["geometry"])
    color = bytes(rgb)
    points = set()
    for row_y in range(top, bottom):
        start = (row_y * image["width"] + left) * 3
        row = image["pixels"][start:start + (right - left) * 3]
        offset = row.find(color)
        while offset >= 0:
            if offset % 3 == 0:
                points.add((left + offset // 3, row_y))
            offset = row.find(color, offset + 1)
    components = []
    while points:
        first = points.pop()
        pending = [first]
        min_x = max_x = first[0]
        min_y = max_y = first[1]
        count = 0
        while pending:
            point_x, point_y = pending.pop()
            count += 1
            min_x, max_x = min(min_x, point_x), max(max_x, point_x)
            min_y, max_y = min(min_y, point_y), max(max_y, point_y)
            for neighbor in ((point_x - 1, point_y), (point_x + 1, point_y),
                             (point_x, point_y - 1), (point_x, point_y + 1)):
                if neighbor in points:
                    points.remove(neighbor)
                    pending.append(neighbor)
        box = (min_x, min_y, max_x + 1, max_y + 1)
        box_width, box_height = box[2] - box[0], box[3] - box[1]
        components.append({
            "bbox": box, "pixels": count,
            "full": (box_width >= 24 and box_height >= 6 and count == box_width * box_height
                     and left < min_x and max_x + 1 < right
                     and top < min_y and max_y + 1 < bottom),
        })
    return sorted(components, key=lambda component: component["bbox"])


def probe_reading_anchor(proc, window, preferences, database_path, args, destination, observer, *,
                         channel, channel_id, other_channel_id, query_target,
                         channel_messages, expected_messages):
    """Prove real wheel/append anchoring at fixed geometry, not body-fraction reflow.

    Native retained-scene reflow coverage lives in transcript_check.cpp. This swatch
    proves actual raster position only; identical explicit foreground/background
    avoids depending on independently colored backgrounds in the SDK's A8 path.
    """
    assert args.zoom == 1 and args.size == (1440, 940), args
    workspace, original_active, _ = save_state(proc, preferences, database_path)
    assert original_active and original_active[0] in (channel_id, "Local/" + query_target), (
        workspace, original_active)
    original_buffer = original_active[0]
    if original_buffer != channel_id:
        select_native_buffer(proc, window, preferences, database_path, args, channel_id)
    layout = wait_layout(proc, preferences)
    assert layout["active_buffer"] == channel_id, layout
    rectangle = layout["conversation"]["rect"]
    bounds = window_geometry(window)
    assert (bounds["WIDTH"], bounds["HEIGHT"]) == (1440, 940), bounds
    settings = tomllib.loads(preferences.read_text())
    assert settings["appearance"]["scale"] == 1, settings
    anchor_rgb, tail_rgb, follow_rgb = (25, 167, 211), (211, 90, 145), (99, 197, 42)
    initial = native_rgb_capture(window)
    for rgb in (anchor_rgb, tail_rgb, follow_rgb):
        assert not native_marker_components(initial, rectangle, rgb), (
            "Marker RGB already present before any real receipt", rgb)
    capture_window(window, destination, "reading-marker-absent")
    nonce = os.urandom(4).hex()
    debug_path = GUI_DEBUG[window][1]
    axis = re.compile(r"wl_pointer[@#]\d+\.axis\(\d+,\s*0,")
    database = sqlite3.connect(f"file:{database_path}?mode=ro", uri=True)

    def unchanged_geometry():
        assert proc.poll() is None, f"GUI exited {proc.returncode} during real reading proof"
        assert window_geometry(window) == bounds, (bounds, window_geometry(window))
        assert read_layout(preferences) == layout, "Conversation geometry changed during the fixed-geometry pair"

    def send(text):
        line = f"PRIVMSG {channel} :{text}"
        assert len(line.encode("utf-8")) <= 510, "Reading fixture exceeded an ordinary IRC line"
        observer.send(line)
        rows = channel_messages(text, query_target)
        assert {row[0] for row in rows} == {channel_id, other_channel_id}, rows
        expected_messages.extend(rows)
        return rows

    def marker(rgb, *, reference=None, below=None):
        previous = None

        def painted():
            nonlocal previous
            unchanged_geometry()
            components = native_marker_components(native_rgb_capture(window), rectangle, rgb)
            if len(components) != 1 or not components[0]["full"]:
                previous = None
                return False
            component = components[0]
            box = component["bbox"]
            stable = previous is not None and all(abs(a - b) <= 1 for a, b in zip(box, previous))
            previous = box
            if reference is not None and any(abs(a - b) > 1 for a, b in zip(box, reference["bbox"])):
                return False
            if below is not None and box[1] <= below["bbox"][1] + 1:
                return False
            return component if stable else False

        return SESSION.until(f"one full stable actual conversation marker {rgb}, reference={reference}, below={below}", painted)

    def screenshot(label, rgb, *, reference=None, below=None):
        component = marker(rgb, reference=reference, below=below)
        path = capture_window(window, destination, label)
        # Confirm the independently encoded PNG was taken in the same stable scene.
        confirmed = marker(rgb, reference=component)
        return confirmed, str(path)


    def frontier():
        return database.execute("SELECT COALESCE(MAX(seq),0) FROM messages").fetchone()[0]

    def exact_append(sequence, rows):
        actual = database.execute(
            "SELECT buffer,nick,text,own,pending FROM messages WHERE seq>? AND buffer IN (?,?) "
            "ORDER BY buffer,nick,text,own,pending",
            (sequence, channel_id, other_channel_id)).fetchall()
        assert actual == sorted(rows), ("New durable real-wire rows differ", rows, actual)

    try:
        ordinary = [
            f"Astra reading {nonce} ordinary {index:02d}: bounded real peer history wraps native "
            "text naturally and leaves enough conversation above the measured body for an actual wheel."
            for index in range(1, 26)
        ]
        anchor_text = (
            f"Astra rich body {nonce} https://example.org/astra/" + "long-url-segment-" * 8 + nonce
            + f" \x0419A7D3,19A7D3Astra{nonce}\x0f ordinary trailing body."
        )
        seeded = []
        for text in ordinary[:22]:
            seeded.extend(send(text))
        seeded.extend(send(anchor_text))
        for text in ordinary[22:]:
            seeded.extend(send(text))
        wait_messages(proc, database_path, expected_messages)
        acknowledged = wait_read_marker(proc, database_path, channel_id)
        assert acknowledged[3] == ordinary[-1], acknowledged
        initial_marker = marker(anchor_rgb)
        box = initial_marker["bbox"]
        move_pointer(window, (box[0] + box[2]) / 2, (box[1] + box[3]) / 2)
        offset = debug_path.stat().st_size
        POINTER.scroll(-1)
        await_native_frame(window, offset, axis)
        before, before_png = screenshot(
            "reading-scrolled-before-append", anchor_rgb, below=initial_marker)
        read_before = database.execute(
            "SELECT read_marker FROM buffers WHERE id=?", (channel_id,)).fetchone()[0]
        assert read_before == acknowledged[0], (acknowledged, read_before)
        sequence = frontier()
        appended = []
        for text in (
                f"Astra reading {nonce} appended ordinary one.",
                f"Astra reading {nonce} appended ordinary two.",
                f"Astra newest real tail \x04D35A91,D35A91Tail{nonce}\x0f."):
            appended.extend(send(text))
        tail_text = text
        wait_messages(proc, database_path, expected_messages)
        exact_append(sequence, appended)
        # A native no-edit command forces a later owned-surface paint without a
        # removed hover toolbar, extra wheel input or transcript geometry change.
        command("end-of-buffer", window=window)
        after, after_png = screenshot("reading-scrolled-after-append", anchor_rgb, reference=before)
        # Message rows persist on arrival; buffers.unread is a Core Save
        # snapshot. Await actual autosave without changing follow/read state.
        save_preferences(proc, preferences, window)
        # This later actual command/frame follows the synchronous Core Save;
        # it neither edits the empty draft nor changes transcript following.
        command("end-of-buffer", window=window)
        marker(anchor_rgb, reference=after)
        saved_read, unread = database.execute(
            "SELECT read_marker,unread FROM buffers WHERE id=?", (channel_id,)).fetchone()
        tail_id, tail_time = database.execute(
            "SELECT id,time FROM messages WHERE buffer=? AND text=?", (channel_id, tail_text)).fetchone()
        assert tail_id and not tail_id.startswith("local:") and tail_time, (tail_id, tail_time)
        expected_unread = sum(row[0] == channel_id for row in appended)
        assert saved_read == read_before and saved_read != tail_time and unread == expected_unread, (
            "Scrolled-up saved marker/unread differs after actual Save",
            read_before, saved_read, tail_time, expected_unread, unread)
        box = after["bbox"]
        move_pointer(window, (box[0] + box[2]) / 2, (box[1] + box[3]) / 2)
        offset = debug_path.stat().st_size
        # One legitimate bounded wheel gesture exceeds the three short appended
        # rows plus the one-notch departure; acknowledgement below proves arrival.
        POINTER.scroll(32)
        await_native_frame(window, offset, axis)
        tail_read = wait_read_marker(proc, database_path, channel_id)
        assert tail_read[2:4] == (tail_id, tail_text), tail_read
        tail_marker, tail_png = screenshot("reading-wheel-restored-tail", tail_rgb)
        assert not native_marker_components(native_rgb_capture(window), rectangle, follow_rgb), (
            "Following marker present before its real incoming append")
        sequence = frontier()
        follow_text = f"Astra following append \x0463C52A,63C52AFollow{nonce}\x0f."
        follow_rows = send(follow_text)
        wait_messages(proc, database_path, expected_messages)
        exact_append(sequence, follow_rows)
        command("end-of-buffer", window=window)
        followed, follow_png = screenshot("reading-bottom-follow-append", follow_rgb)
        follow_read = wait_read_marker(proc, database_path, channel_id)
        assert follow_read[3] == follow_text and follow_read[2] and not follow_read[2].startswith("local:"), follow_read
        report = {
            "conversation": rectangle, "window": bounds, "ordinary_seed_messages": 25,
            "new_durable_seed_copies": len(seeded), "new_durable_append_copies": len(appended) + len(follow_rows),
            "initial_marker": initial_marker, "scrolled_before": before, "scrolled_after": after,
            "raster_rounding_tolerance": 1, "read_before": read_before, "read_after_append": saved_read,
            "unread_after_append": unread, "tail_read": tail_read, "following_read": follow_read,
            "tail_marker": tail_marker, "following_marker": followed,
            "screenshots": [before_png, after_png, tail_png, follow_png],
            "remote_history_prepend": "UNEXERCISED: no demonstrated remote-only frontier",
            "loaded_reply_reaction": "UNEXERCISED: this focused scenario sends only ordinary PRIVMSG",
            "limits": "Fixed-geometry actual raster proof, not normalized body-fraction reflow or arbitrary background-color fidelity",
        }
        print("Native real-wire reading anchor:", json.dumps(report, ensure_ascii=False), flush=True)
    finally:
        database.close()
    if original_buffer != channel_id:
        select_native_buffer(proc, window, preferences, database_path, args, original_buffer)
    return report


def probe_transcript_surfaces(proc, window, preferences, database_path, args, destination, observer, *,
                              channel, channel_id, other_channel_id, query_target, query_id,
                              own_nick, channel_messages, expected_messages, cleanup, lab, receive):
    """Exercise real body selection, CTCP ACTION/day, and native Smart activity expansion."""
    original = save_state(proc, preferences, database_path)
    assert original[1] == (query_id,), original
    _composer_native(window, "", 0, 0)
    settings = tomllib.loads(preferences.read_text())
    original_override = settings["presence"]["channels"].get(channel_id)
    assert original_override in (None, "all", "smart", "hidden"), settings["presence"]
    database = sqlite3.connect(f"file:{database_path}?mode=ro", uri=True)
    nonce = os.urandom(4).hex()
    debug_path = GUI_DEBUG[window][1]
    release = re.compile(r"wl_pointer[@#]\d+\.button\(\d+,\s*\d+,\s*272,\s*0\)")
    axis = re.compile(r"wl_pointer[@#]\d+\.axis\(\d+,\s*0,")
    screenshots, sent_wire, received_wire = [], [], []
    original_caps = set(observer.capabilities)
    added_caps = []
    start = len(expected_messages)
    presence_changed = False
    original_mode = "irc" if query_id in settings["presentation"]["irc_channels"] else "bubbles"
    display_changed = False

    def photo(label):
        path = capture_window(window, destination, "transcript-" + label)
        screenshots.append(str(path))
        return str(path)

    def send(target, text, *, decoded=None):
        line = f"PRIVMSG {target} :{text}"
        assert len(line.encode("utf-8")) <= 510, line
        observer.send(line)
        sent_wire.append(line)
        receive("PRIVMSG", [target, text], query_target)
        frame = observer.frames[-1]
        received_wire.append(frame)
        rows = ([(query_id, query_target, text if decoded is None else decoded, 0, 0)]
                if target == own_nick else channel_messages(text, query_target))
        expected_messages.extend(rows)
        wait_messages(proc, database_path, expected_messages)
        return frame

    def markers(rectangle, *colors):
        previous = None

        def painted():
            nonlocal previous
            assert proc.poll() is None, "GUI exited during actual transcript surface capture"
            image = native_rgb_capture(window)
            components = [native_marker_components(image, rectangle, color) for color in colors]
            if any(len(items) != 1 or not items[0]["full"] for items in components):
                previous = None
                return False
            boxes = [items[0]["bbox"] for items in components]
            stable = previous is not None and all(
                abs(a - b) <= 1 for box, old in zip(boxes, previous) for a, b in zip(box, old))
            previous = boxes
            return boxes if stable else False

        return SESSION.until("complete stable real transcript marker bounds " + repr(colors), painted)

    def wheel(rectangle, steps):
        x, y, width, height = rectangle
        move_pointer(window, x + width / 2, y + height / 2)
        offset = debug_path.stat().st_size
        POINTER.scroll(steps)
        await_native_frame(window, offset, axis)

    def presence(mode):
        layout = read_layout(preferences)
        root = open_title_menu(window, title_actions(layout, args.zoom))
        child = open_native_submenu(window, root, 3, from_end=True)
        capture_native_popup(window, child, destination, "transcript-presence-" + mode)
        index = ("global", "all", "smart", "hidden").index(mode)
        popup_keys(window, root, "Home", *(["Down"] * index), "Return")
        expected = dict(settings["presence"]["channels"])
        if mode == "global":
            expected.pop(channel_id, None)
        else:
            expected[channel_id] = mode
        wait_preference(proc, preferences, "presence", "channels", expected)

    try:
        # Negotiate on the real existing TLS socket, without changing its identity.
        observer.send("CAP LS 302")
        available = set()
        while True:
            _, params, _ = observer.wait(
                lambda command, params, prefix: command == "CAP" and "LS" in params, timeout=15)
            available.update(item.split("=", 1)[0] for item in params[-1].split())
            if len(params) < 4 or params[-2] != "*":
                break
        required = {"echo-message", "server-time", "message-tags"}
        assert required <= available, ("Actual peer lacks required wire evidence capabilities", available)
        added_caps = sorted(required - original_caps)
        if added_caps:
            observer.send("CAP REQ :" + " ".join(added_caps))
            observer.wait(lambda command, params, prefix:
                          command == "CAP" and ("ACK" in params or "NAK" in params), timeout=15)
        assert required <= observer.capabilities, ("Missing actual CAP ACK", observer.frames[-1])

        if original_mode != "bubbles":
            set_message_style(proc, window, preferences, database_path, args, destination,
                              query_id, "bubbles", label="transcript-query-action-bubbles")
            display_changed = True
        layout = read_layout(preferences)
        rectangle = layout["conversation"]["rect"]
        left_rgb, right_rgb, action_rgb = (42, 185, 113), (179, 79, 213), (213, 143, 35)
        initial = native_rgb_capture(window)
        for color in (left_rgb, right_rgb, action_rgb):
            assert not native_marker_components(initial, rectangle, color), color
        logical = " café e\u0301 \x02bold\x02 \x1ditalic\x1d 中 "
        decoded = " café e\u0301 bold italic 中 "
        body = "\x042AB971,2AB971LEFTLEFT\x0f" + logical + "\x04B34FD5,B34FD5RGHTRGHT\x0f"
        body_frame = send(own_nick, body)
        wait_read_marker(proc, database_path, query_id)
        left, right = markers(rectangle, left_rgb, right_rgb)
        assert left[2] < right[0] and abs(left[1] - right[1]) <= 1 and abs(left[3] - right[3]) <= 1, (
            "Selection fixture must occupy one actual shaped line", left, right)
        draft = "Astra protected draft " + nonce
        type_text(draft)
        framed_keys(window, "Home", *(["shift+Right"] * 4))
        before = observe_native_input(window, destination, "transcript-original-composer-selection",
                                      draft, 4, 0)
        # Native Composer growth may move the transcript: locate again, never estimate advances.
        left, right = markers(rectangle, left_rgb, right_rgb)
        assert abs(left[1] - right[1]) <= 1 and abs(left[3] - right[3]) <= 1, (left, right)
        seed_clipboard("Unrelated clipboard " + nonce, cleanup)
        y = (left[1] + left[3]) / 2
        drag_start, drag_end = (left[2] + .5, y), (right[0] - .5, y)
        move_pointer(window, *drag_start)
        offset = debug_path.stat().st_size
        pointer_button(True)
        move_pointer(window, *drag_end)
        pointer_button(False)
        await_native_frame(window, offset, release)
        assert native_input_state(window) == before, "Body drag changed original Composer text/selection/focus"
        photo("body-drag-selected")
        offset = debug_path.stat().st_size
        presses = key("ctrl+c", trace=True)
        final = presses[-1]
        released = re.compile(rf"wl_keyboard[@#]\d+\.key\(\d+,\s*\d+,\s*{final},\s*0\)")
        SESSION.until("actual copy key release", lambda: released.search(
            debug_path.read_bytes()[offset:].decode(errors="replace")))
        SESSION.until("exact decoded body text in actual wl-paste", lambda: clipboard_text() == decoded)
        after_copy = native_input_state(window)
        assert after_copy == before, "Transcript Ctrl-C changed protected Composer state"
        copied = clipboard_text()
        assert copied == decoded, (copied, decoded)
        copy_png = photo("body-copy-exact")
        # The real Send button routes the untouched original draft, despite its selection.
        targets = composer_primary_targets(read_layout(preferences), args.zoom, before["caret"])
        offset = debug_path.stat().st_size
        click(window, *targets["send"])
        await_native_frame(window, offset, release)
        receive("PRIVMSG", [query_target, draft], own_nick)
        received_wire.append(observer.frames[-1])
        expected_messages.append((query_id, own_nick, draft, 1, 0))
        wait_messages(proc, database_path, expected_messages)
        _composer_native(window, "", 0, 0)

        action_text = f"waves \x04D58F23,D58F23Action{nonce}\x0f across the real transcript"
        action_frame = send(own_nick, "\x01ACTION " + action_text + "\x01", decoded=action_text)
        action_rows = database.execute(
            "SELECT id,time,timestamp,nick,text,action,presence,pending,deleted FROM messages "
            "WHERE buffer=? AND text=?", (query_id, action_text)).fetchall()
        assert len(action_rows) == 1, action_rows
        action = action_rows[0]
        assert action[0] and not action[0].startswith("local:") and action[1] and action[2] > 0, action
        assert action[3:] == (query_target, action_text, 1, 0, 0, 0), action
        assert action_frame["tags"]["time"] == action[1], (action_frame, action)
        if "msgid" in action_frame["tags"]:
            assert action_frame["tags"]["msgid"] == action[0], (action_frame, action)
        wait_read_marker(proc, database_path, query_id)
        action_box, = markers(rectangle, action_rgb)
        action_png = photo("action-and-day")
        # This fresh four-row query fits the real first-day chip and action together.
        # Record actual stored local day, not a synthetic timestamp or an OCR claim.
        first_timestamp = database.execute(
            "SELECT timestamp FROM messages WHERE buffer=? ORDER BY sort_time,seq LIMIT 1",
            (query_id,)).fetchone()[0]
        assert time.strftime("%Y-%m-%d", time.localtime(first_timestamp)) == time.strftime(
            "%Y-%m-%d", time.localtime(action[2])), (first_timestamp, action)
        query_count = database.execute("SELECT COUNT(*) FROM messages WHERE buffer=?", (query_id,)).fetchone()[0]
        assert query_count == 4, ("ACTION/day fixture unexpectedly contains hidden extra history", query_count)
        day_png = photo("first-day-chip")
        assert native_input_state(window) == _composer_native(window, "", 0, 0)
        if display_changed:
            set_message_style(proc, window, preferences, database_path, args, destination,
                              query_id, original_mode, label="transcript-query-original-display")
            display_changed = False

        layout = select_native_buffer(proc, window, preferences, database_path, args, channel_id)
        presence_changed = True
        presence("smart")
        rectangle = layout["conversation"]["rect"]
        pre_rgb, post_rgb = (45, 155, 217), (217, 69, 119)
        initial = native_rgb_capture(window)
        for color in (pre_rgb, post_rgb):
            assert not native_marker_components(initial, rectangle, color), color
        pre_text = f"\x042D9BD9,2D9BD9Before{nonce}\x0f"
        post_text = f"\x04D94577,D94577After{nonce}\x0f"
        send(channel, pre_text)
        pre_read = wait_read_marker(proc, database_path, channel_id)
        assert pre_read[3] == pre_text, pre_read
        frontier = database.execute("SELECT COALESCE(MAX(seq),0) FROM messages").fetchone()[0]
        guest_nick, renamed = "sg" + nonce, "sr" + nonce
        reason = "surface " + nonce
        guest_frames = []
        # Register cleanup before the first JOIN; close only after the actual PART.
        with fixtures.BootstrapClient(
                dict(lab.endpoints["direct"], nick=guest_nick, username=guest_nick),
                lab.ca_file, authenticate=False) as guest:
            for line, command_name, params, nick in (
                    ("JOIN " + channel, "JOIN", [channel], guest_nick),
                    ("NICK " + renamed, "NICK", [renamed], guest_nick),
                    (f"PART {channel} :{reason}", "PART", [channel, reason], renamed)):
                guest.send(line)
                sent_wire.append(line)
                receive(command_name, params, nick)
                guest_frames.append(observer.frames[-1])
        details = [(guest_nick, f"{guest_nick} joined {channel}", 1),
                   (guest_nick, f"{guest_nick} is now known as {renamed}", 4),
                   (renamed, f"{renamed} left {channel} — {reason}", 2)]
        presence_query = (
            "SELECT nick,text,activity FROM messages WHERE buffer=? AND seq>? AND presence=1 ORDER BY seq")
        wait_rows(proc, database_path, presence_query, (channel_id, frontier), details)
        wait_rows(proc, database_path, presence_query, (other_channel_id, frontier), details)
        send(channel, post_text)
        post_read = wait_read_marker(proc, database_path, channel_id)
        assert post_read[3] == post_text, post_read
        collapsed = markers(rectangle, pre_rgb, post_rgb)
        pre, post = collapsed
        assert pre[3] < post[1], collapsed
        collapsed_png = photo("smart-collapsed")
        # Root summary area spans the full chat width. These actual one-line
        # flanks bound exactly one summary row, not an avatar/text-width guess.
        x, _, width, _ = rectangle
        summary_click = (x + width / 2, (pre[3] + post[1]) / 2)
        offset = debug_path.stat().st_size
        click(window, *summary_click)
        await_native_frame(window, offset, release)
        wheel(rectangle, 32)
        expanded = markers(rectangle, pre_rgb, post_rgb)
        before_distance = collapsed[1][1] - collapsed[0][1]
        after_distance = expanded[1][1] - expanded[0][1]
        assert after_distance > before_distance + 6, (
            "Actual Smart chip click did not expand the three original detail rows", collapsed, expanded)
        expanded_png = photo("smart-expanded-original-details")
        assert database.execute(presence_query, (channel_id, frontier)).fetchall() == details, (
            "Expansion changed original received presence details")
        presence_rows = database.execute(
            "SELECT buffer,id,time,timestamp,nick,text,activity FROM messages "
            "WHERE seq>? AND presence=1 AND buffer IN (?,?) ORDER BY buffer,seq",
            (frontier, channel_id, other_channel_id)).fetchall()
        assert all(row[2] and row[3] > 0 for row in presence_rows), presence_rows
        assert [(row[0], *row[4:]) for row in presence_rows] == [
            (buffer, *detail) for buffer in sorted((channel_id, other_channel_id)) for detail in details
        ], ("Missing or altered real lifecycle on one channel connection", presence_rows)
        assert len({time.strftime("%Y-%m-%d", time.localtime(row[3])) for row in presence_rows}) == 1, (
            "Smart activity fixture crossed a real local-day boundary", presence_rows)
        presence(original_override or "global")
        presence_changed = False
        select_native_buffer(proc, window, preferences, database_path, args, query_id)
        _composer_native(window, "", 0, 0)
        final = save_state(proc, preferences, database_path)
        assert final[:2] == original[:2], (original, final)
        assert saved_workspace_intent(tomllib.loads(preferences.read_text())) == saved_workspace_intent(settings)
        report = {
            "body": {"wire": body_frame, "left_marker": left, "right_marker": right,
                     "drag_start": drag_start, "drag_end": drag_end, "clipboard": copied,
                     "composer_before": before, "composer_after_copy": after_copy, "copy_png": copy_png,
                     "untouched_draft_sent": draft},
            "action": {"wire": action_frame, "stored": action, "body_marker": action_box,
                       "local_day": time.strftime("%Y-%m-%d", time.localtime(action[2])),
                       "query_rows": query_count, "action_png": action_png, "day_png": day_png},
            "smart": {"guest_wire": guest_frames, "original_details": details,
                      "stored_presence": presence_rows, "pre_read": pre_read, "post_read": post_read,
                      "collapsed": collapsed, "click": summary_click, "expanded": expanded,
                      "collapsed_distance": before_distance, "expanded_distance": after_distance,
                      "collapsed_png": collapsed_png, "expanded_png": expanded_png},
            "sent_wire": sent_wire, "received_wire": received_wire,
            "exact_new_nonpresence_rows": expected_messages[start:], "screenshots": screenshots,
            "limits": "PNG evidence is actual rendered output, not OCR; source/native checks own chip text/style geometry.",
        }
        evidence = destination.with_name(destination.stem + "-transcript-surfaces.json")
        evidence.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
        print("Native transcript surfaces:", json.dumps(report, ensure_ascii=False), flush=True)
        return report
    finally:
        if presence_changed:
            presence(original_override or "global")
        if display_changed:
            select_native_buffer(proc, window, preferences, database_path, args, query_id)
            set_message_style(proc, window, preferences, database_path, args, destination,
                              query_id, original_mode, label="transcript-query-display-restored")
        if added_caps:
            observer.send("CAP REQ :" + " ".join("-" + cap for cap in added_caps))
            observer.wait(lambda command, params, prefix:
                          command == "CAP" and ("ACK" in params or "NAK" in params), timeout=15)
            assert observer.capabilities == original_caps, observer.capabilities
        database.close()


def probe_settings_slider_wheel(window, preferences, zoom, destination):
    """Locate the native focused thumb by raster, then wheel over that exact thumb."""
    from PIL import Image, ImageChops

    x, y, width, height = read_layout(preferences)["settings"]["rect"]
    # Wide Settings has a 200-unit category rail; exclude it and sticky chrome.
    crop = (round(x + 224 * zoom), round(y + 64 * zoom),
            round(x + width - 16 * zoom), round(y + height - 60 * zoom))

    def image():
        sample = native_rgb_capture(window)
        return Image.frombytes("RGB", (sample["width"], sample["height"]), sample["pixels"])

    framed_keys(window, "Tab", "Tab")
    toggle = image()
    framed_keys(window, "Tab")  # Native opacity slider, from category navigation.
    focused = image()
    difference = ImageChops.difference(toggle, focused).crop(crop).convert("L")
    rows = [row for row in range(difference.height)
            if difference.crop((0, row, difference.width, row + 1)).getbbox()]
    assert rows, "Native slider focus thumb was not rendered"
    # The previous Toggle's ring is above the slider. The last changed-row band
    # identifies the actual thumb, not an inferred control/label rectangle.
    first = rows[-1]
    while first - 1 in rows:
        first -= 1
    thumb = difference.crop((0, first, difference.width, rows[-1] + 1)).getbbox()
    assert thumb is not None
    point = (crop[0] + (thumb[0] + thumb[2]) / 2,
             crop[1] + (first + rows[-1] + 1) / 2)
    assert thumb[2] - thumb[0] <= 80 * zoom and rows[-1] - first <= 80 * zoom, (
        "Ambiguous native slider focus raster", thumb, rows)
    before_path = capture_window(window, destination, "settings-slider-wheel-before")
    move_pointer(window, *point)
    marker = GUI_DEBUG[window][1].stat().st_size
    POINTER.scroll(4)
    await_native_frame(window, marker, re.compile(r"wl_pointer[@#]\d+\.axis\("))

    def scrolled():
        changed = ImageChops.difference(focused.crop(crop), image().crop(crop)).convert("L")
        count = sum(value != 0 for value in changed.getdata())
        return count if count > 1000 * zoom * zoom else False

    changed = SESSION.until("wheel over native slider moved its parent detail content", scrolled)
    after_path = capture_window(window, destination, "settings-slider-wheel-after")
    framed_keys(window, "shift+Tab", "shift+Tab", "shift+Tab")
    destination.with_name(destination.stem + "-settings-slider-wheel.json").write_text(json.dumps({
        "pointer": point, "detail_crop": crop, "changed_detail_pixels": changed,
        "captures": [str(before_path), str(after_path)],
        "oracle": "actual focused thumb raster, axis/root paint and changed detail content; saved values checked by caller",
    }, indent=2) + "\n")


def probe_settings_categories(proc, window, preferences, config, database_path, args, destination, *,
                              observer, query_target, query_id, own_nick, cleanup, lab, receive):
    baseline = tomllib.loads(preferences.read_text())
    profiles = tomllib.loads(config.read_text())
    identities = profiles["identity"]
    servers = profiles["server"]
    names = [server["name"] for server in servers]
    assert set(names) == {"Local", "Other"} and len(names) == 2, names
    local = next(server for server in servers if server["name"] == "Local")
    other = next(server for server in servers if server["name"] == "Other")
    identity_names = [identity["name"] for identity in identities]
    assert len(identities) >= 2 and local["identity"] != other["identity"], identity_names
    local_index = identity_names.index(local["identity"])
    other_index = identity_names.index(other["identity"])
    local_identity = identities[local_index]
    other_nick = identities[other_index]["nick"]
    assert local_identity["nick"] == own_nick and query_id == f"Local/{query_target}"
    channel = local["channels"][0]
    database = sqlite3.connect(f"file:{database_path}?mode=ro", uri=True)
    history_query = "SELECT * FROM messages WHERE presence=0 ORDER BY buffer,sort_time,seq"
    buffer_query = "SELECT id,server,target,kind,draft FROM buffers ORDER BY id"
    new_rows = []

    def save(label, expected_profiles=profiles):
        saved = save_settings_footer(window, saved_path=preferences,
                                     destination=destination, label=label)
        assert tomllib.loads(config.read_text()) == expected_profiles, "Native Save changed unexpected profile fields"
        return saved

    def roundtrip(label):
        token = f"astra-settings-{label}-{time.monotonic_ns()}"
        observer.send("PING :" + token)
        observer.wait(lambda actual, params, prefix:
                      actual == "PONG" and params and params[-1] == token, timeout=15)
        return len(observer.frames)

    def no_lifecycle(start, nicks):
        changed = [frame for frame in observer.frames[start:]
                   if frame["command"] in ("QUIT", "JOIN") and
                   frame["prefix"].split("!", 1)[0] in nicks]
        assert not changed, lab._redact(repr(changed))

    def known_row(text, nick, own):
        wait_messages(proc, database_path, [(query_id, nick, text, own, 0)])
        rows = database.execute(
            "SELECT buffer,id,nick,text,own,pending,deleted FROM messages WHERE text=?",
            (text,),
        ).fetchall()
        assert len(rows) == 1, lab._redact(repr(rows))
        row = rows[0]
        assert row[0] == query_id and row[2:] == (nick, text, own, 0, 0), lab._redact(repr(row))
        assert row[1] and not row[1].startswith("local:"), row[1]
        new_rows.append(row)
        return row

    def feedback_top(label, modal):
        # Move focus off/on the existing Connect action after the Core SQLite
        # barrier, then genuinely wheel the detail viewport to its feedback.
        framed_keys(window, "shift+Tab", "Tab")
        x, y, width, height = modal["rect"]
        move_pointer(window, x + width - 20 * args.zoom, y + height / 2)
        offset = GUI_DEBUG[window][1].stat().st_size
        POINTER.scroll(-100)
        await_native_frame(window, offset, re.compile(r"wl_pointer[@#]\d+\.axis\("))
        capture_window(window, destination, label)

    try:
        wait_read_marker(proc, database_path, query_id)
        original_buffers = database.execute(buffer_query).fetchall()
        original_draft = next(row[4] for row in original_buffers if row[0] == query_id)
        history = database.execute(history_query)
        history_columns = [column[0] for column in history.description]
        history_before = history.fetchall()
        other_state = database.execute(
            "SELECT id,unread,read_marker,draft FROM buffers WHERE server='Other' ORDER BY id"
        ).fetchall()
        wire_start = roundtrip("before-ui")
        open_settings(window, preferences)
        framed_keys(window, "shift+Tab")
        categories = ("appearance", "keyboard", "chat", "effects", "sounds", "networks", "identities")
        for index, category in enumerate(categories):
            settings_category(window, args.zoom, index)
            capture_window(window, destination, "settings-wide-category-" + category)
            if category == "appearance":
                stops = 1
                source = baseline["appearance"]["theme_source"]
                source_index = {"auto": 0, "builtin": 1, "built-in": 1, "noctalia": 2}[source]
                alternate = 0 if source_index == 1 else 1
                for selected, expected in ((alternate, ("auto", "builtin")[alternate]),
                                           (source_index, source)):
                    framed_keys(window, *(["Tab"] * stops))
                    popup = open_native_popup(window, lambda: key("Return"))
                    capture_window(window, destination, f"settings-theme-source-dropdown-{selected}")
                    popup_keys(window, popup, "Home", *(["Down"] * selected), "Return", root_changed=False)
                    framed_keys(window, "Tab", "shift+Tab")
                    framed_keys(window, *(["shift+Tab"] * stops))
                    save(f"settings-theme-source-saved-{selected}")
                    wait_preference(proc, preferences, "appearance", "theme_source", expected)
                modal = wait_layout(proc, preferences, ("settings",))["settings"]
                x, y, width, height = modal["rect"]
                bounds = window_geometry(window)
                assert tomllib.loads(preferences.read_text())["workspace"]["native_dialog"] == "settings"
                assert native_input_state(window) is None, "Settings navigation did not own native non-text focus"
                assert 0 <= x and 0 <= y, modal
                assert x + width <= bounds["WIDTH"] and y + height <= bounds["HEIGHT"], modal
                probe_settings_slider_wheel(window, preferences, args.zoom, destination)
                assert save("settings-slider-wheel-values-unchanged")["appearance"] == baseline["appearance"]
            elif category == "keyboard":
                assert "complete" in baseline["bindings"]
                framed_keys(window, "Tab", "Tab", "Return")
                framed_keys(window, "F12", "Tab", "Escape")
                capture_window(window, destination, "settings-key-recording-captures-tab-escape")
                framed_keys(window, "ctrl+g", "shift+Tab", "End")
                chord = baseline["bindings"]["complete"]
                end = len(chord.encode("utf-8"))
                observe_native_input(window, destination, "settings-key-record-cancelled", chord, end, end)
                framed_keys(window, "shift+Tab")
                assert save("settings-key-record-cancel-saved")["bindings"] == baseline["bindings"]
                framed_keys(window, "Tab", "Tab", "Return", "F12", "Tab", "Escape", "ctrl+Return")
                framed_keys(window, "shift+Tab", "End")
                custom = "f12 tab escape"
                end = len(custom.encode("utf-8"))
                observe_native_input(window, destination, "settings-key-current-configured-chord-help", custom, end, end)
                framed_keys(window, "shift+Tab")
                saved = save("settings-key-record-commit-saved")
                assert saved["bindings"] == {**baseline["bindings"], "complete": custom}, saved["bindings"]
                # Exercise the ACTUAL rebound prefix and mode line outside the
                # recorder; a screenshot of the binding field is not that proof.
                framed_keys(window, "ctrl+g")
                command("end-of-buffer", window=window)
                draft_end = len(original_draft.encode("utf-8"))
                _composer_native(window, original_draft, draft_end, draft_end)
                framed_keys(window, "F12")
                capture_window(window, destination, "settings-rebound-prefix-f12-mode-line")
                _composer_native(window, original_draft, draft_end, draft_end)
                framed_keys(window, "Tab")
                capture_window(window, destination, "settings-rebound-prefix-f12-tab-mode-line")
                _composer_native(window, original_draft, draft_end, draft_end)
                framed_keys(window, "Escape")
                _composer_native(window, original_draft, draft_end, draft_end)
                open_settings(window, preferences)
                framed_keys(window, "shift+Tab")
                settings_category(window, args.zoom, 1)
                framed_keys(window, "Tab")
                settings_field(window, chord, destination, "settings-key-original-restored-in-real-field")
                framed_keys(window, "shift+Tab")
                assert save("settings-key-original-restored-saved")["bindings"] == baseline["bindings"]
            elif category in ("chat", "effects", "sounds"):
                stops, section, name = {
                    "chat": (3, "typing", "receive"),
                    "effects": (2, "appearance", "reduced_motion"),
                    "sounds": (1, "audio", "enabled"),
                }[category]
                original = baseline[section][name]
                for value in (not original, original):
                    framed_keys(window, *(["Tab"] * stops), "Return")
                    capture_window(window, destination, f"settings-{category}-{str(value).lower()}")
                    framed_keys(window, *(["shift+Tab"] * stops))
                    save(f"settings-{category}-{str(value).lower()}-saved")
                    wait_preference(proc, preferences, section, name, value)
                if category == "effects":
                    # Independent native selectors: exercise each offered value,
                    # not a Cartesian product of complete messaging workflows.
                    for field, stop, choices in (
                            ("send_motion", 3, ("none", "flight", "rise", "pop")),
                            ("send_reveal", 4, ("none", "fade", "pixels"))):
                        original = baseline["appearance"][field]
                        for choice in (*choices, original):
                            framed_keys(window, *(["Tab"] * stop))
                            settings_select(window, choices.index(choice))
                            framed_keys(window, *(["shift+Tab"] * stop))
                            saved = save(f"settings-{field}-{choice}")
                            expected = {**baseline["appearance"], field: choice}
                            assert saved["appearance"] == expected, (field, choice, saved["appearance"])
            else:
                framed_keys(window, "shift+Tab", "shift+Tab", "shift+Tab", "Return")
                capture_window(window, destination, "settings-" + category + "-remove-confirmation")
                framed_keys(window, "Return")  # Initially focused actual Cancel.
                framed_keys(window, "Tab", "Tab", "Tab")
                save("settings-" + category + "-remove-cancelled")

        bounds = window_geometry(window)
        assert native_window(window)["floating"] in ("auto_off", "user_off"), native_window(window)
        try:
            SESSION.ipc("command", f"[con_id={window}] floating enable, resize set 720 px 480 px, move position center")
            SESSION.until("actual compact Settings client configure", lambda:
                          (window_geometry(window)["WIDTH"], window_geometry(window)["HEIGHT"]) == (720, 480))
            framed_keys(window, "Tab", "shift+Tab")
            for index, category in enumerate(categories):
                settings_category(window, args.zoom, index, destination)
                capture_window(window, destination, "settings-compact-category-" + category)
        finally:
            SESSION.ipc("command", f"[con_id={window}] floating disable")
            SESSION.until("restored actual wide Settings client configure", lambda:
                          window_geometry(window) == bounds)
        framed_keys(window, "Tab", "shift+Tab")

        # Share a real identity through the Other network's actual selector.
        settings_category(window, args.zoom, 5)
        framed_keys(window, "Tab")
        settings_select(window, names.index("Other"))
        framed_keys(window, *(["Tab"] * 10))  # Includes Display alias and Avatar image path.
        settings_select(window, local_index)  # Rebuild focuses Network selector.
        framed_keys(window, *(["Tab"] * 10))  # Network selector -> Shared identity profile.
        capture_window(window, destination, "settings-network-identity-used-by-two")
        framed_keys(window, "Tab", "Return")  # Edit identity -> actual Identities Profile selector.
        capture_window(window, destination, "settings-identity-used-by-two")
        shared_realname = local_identity["realname"] + " [Astra shared identity]"
        framed_keys(window, *(["Tab"] * 5))
        settings_field(window, shared_realname, destination, "settings-shared-identity-realname-edited")
        framed_keys(window, *(["shift+Tab"] * 6))
        shared_profiles = {
            **profiles,
            "server": [{**server, "identity": local["identity"]} if server["name"] == "Other" else server
                       for server in servers],
            "identity": [{**identity, "realname": shared_realname} if index == local_index else identity
                         for index, identity in enumerate(identities)],
        }
        save("settings-shared-identity-saved", shared_profiles)

        # Restore both original references and the shared real name before any reconnect.
        settings_category(window, args.zoom, 5)
        framed_keys(window, "Tab")
        settings_select(window, names.index("Other"))
        framed_keys(window, *(["Tab"] * 10))
        settings_select(window, other_index)
        framed_keys(window, "shift+Tab")
        settings_category(window, args.zoom, 6)
        framed_keys(window, "Tab")
        settings_select(window, local_index)
        framed_keys(window, *(["Tab"] * 5))
        settings_field(window, local_identity["realname"], destination, "settings-shared-original-realname-restored")
        framed_keys(window, *(["shift+Tab"] * 6))
        save("settings-original-identity-references-restored")
        roundtrip("shared-saved")
        no_lifecycle(wire_start, {own_nick, other_nick})
        assert database.execute(buffer_query).fetchall() == original_buffers
        assert database.execute(history_query).fetchall() == history_before

        config_bytes, preference_bytes = config.read_bytes(), preferences.read_bytes()
        failure_realname = local_identity["realname"] + " [Astra save failure recovery]"
        edited_profiles = {
            **profiles,
            "identity": [{**identity, "realname": failure_realname} if index == local_index else identity
                         for index, identity in enumerate(identities)],
        }
        incoming_text = f"Astra Settings Core failure barrier {time.monotonic_ns()}"
        outgoing_text = f"Astra Settings still-live Local barrier {time.monotonic_ns()}"
        failure_start = roundtrip("before-readonly-save-connect")
        config.parent.chmod(0o555)
        try:
            framed_keys(window, "Tab")  # Existing Identities Profile selector.
            settings_select(window, local_index)
            framed_keys(window, *(["Tab"] * 5))
            settings_field(window, failure_realname, destination, "settings-readonly-live-edit-retained")
            framed_keys(window, *(["shift+Tab"] * 6))
            settings_category(window, args.zoom, 5)
            framed_keys(window, "Tab")
            settings_select(window, names.index("Local"))
            framed_keys(window, "shift+Tab")
            framed_keys(window, *(["Tab"] * 15), "Return")  # Category -> Save and connect, including alias/avatar.
            # Actual Core reduction/durable receipt, not worker PING/PONG, orders
            # the following native root paint after the failed Save action.
            observer.send(f"PRIVMSG {own_nick} :{incoming_text}")
            known_row(incoming_text, query_target, 0)
            feedback_top("settings-save-reconnect-private-filesystem-error", modal)
            framed_keys(window, *(["shift+Tab"] * 12), "End")  # Connect -> editable Network name.
            observe_native_input(window, destination, "settings-error-form-remains-editable", "Local", 5, 5)
            assert config.read_bytes() == config_bytes, "Failed native save changed private profiles"
            assert preferences.read_bytes() == preference_bytes, "Failed native save changed preferences"
            assert database.execute(buffer_query).fetchall() == original_buffers
            framed_keys(window, "ctrl+g")
            command("end-of-buffer", window=window)
            end = len(original_draft.encode("utf-8"))
            observe_native_input(window, destination, "settings-failed-save-original-conversation-draft",
                                 original_draft, end, end)
            command("beginning-of-buffer", window=window)
            command("set-mark", window=window)
            command("end-of-buffer", window=window)
            command("kill-region", window=window)
            seed_clipboard(outgoing_text, cleanup)
            framed_keys(window, "ctrl+v")
            end = len(outgoing_text.encode("utf-8"))
            observe_native_input(window, destination, "settings-live-connection-barrier-draft",
                                 outgoing_text, end, end)
            command("send", window=window)
            receive("PRIVMSG", [query_target, outgoing_text], own_nick)
            outgoing_row = known_row(outgoing_text, own_nick, 1)
            wire = [frame for frame in observer.frames[failure_start:]
                    if frame["command"] == "PRIVMSG" and frame["params"] == [query_target, outgoing_text]
                    and frame["prefix"].split("!", 1)[0] == own_nick]
            assert len(wire) == 1, lab._redact(repr(wire))
            if wire[0]["tags"].get("msgid"):
                assert outgoing_row[1] == wire[0]["tags"]["msgid"], (outgoing_row[1], wire[0]["tags"])
            no_lifecycle(failure_start, {own_nick, other_nick})
            seed_clipboard(original_draft, cleanup)
            framed_keys(window, "ctrl+v")
            end = len(original_draft.encode("utf-8"))
            observe_native_input(window, destination, "settings-live-barrier-original-draft-restored",
                                 original_draft, end, end)
            assert config.read_bytes() == config_bytes and preferences.read_bytes() == preference_bytes
        finally:
            config.parent.chmod(0o700)

        # Save the retained, genuinely changed private field BEFORE reconnecting.
        open_settings(window, preferences)
        framed_keys(window, "shift+Tab")
        settings_category(window, args.zoom, 5)
        framed_keys(window, "Tab")
        settings_select(window, names.index("Local"))
        framed_keys(window, "shift+Tab")
        reconnect_start = len(observer.frames)
        framed_keys(window, *(["Tab"] * 15), "Return")
        lifecycle = []

        def rejoined(actual, params, prefix):
            nick = prefix.split("!", 1)[0]
            if actual not in ("QUIT", "JOIN") or nick not in (own_nick, other_nick):
                return False
            assert nick == own_nick, lab._redact(repr((actual, params, prefix)))
            assert tomllib.loads(config.read_text()) == edited_profiles, "Reconnect preceded the changed private-profile save"
            lifecycle.append(actual)
            return actual == "JOIN" and params == [channel]

        observer.wait(rejoined, timeout=15)
        assert lifecycle[0] == "QUIT" and lifecycle[-1] == "JOIN", lifecycle
        no_lifecycle(reconnect_start, {other_nick})
        feedback_top("settings-save-reconnect-recovered-success", modal)
        # Only this deliberate successful request reconnects Local; Other stays live.
        roundtrip("after-successful-reconnect")
        no_lifecycle(wire_start, {other_nick})
        assert database.execute(
            "SELECT id,unread,read_marker,draft FROM buffers WHERE server='Other' ORDER BY id"
        ).fetchall() == other_state

        framed_keys(window, *(["shift+Tab"] * 15))  # Connect -> category navigation.
        settings_category(window, args.zoom, 6)
        framed_keys(window, "Tab")
        settings_select(window, local_index)
        framed_keys(window, *(["Tab"] * 5))
        settings_field(window, local_identity["realname"], destination, "settings-recovery-original-profile-restored")
        framed_keys(window, *(["shift+Tab"] * 6))
        save("settings-all-original-profiles-restored")
        framed_keys(window, "ctrl+g")
        save_state(proc, preferences, database_path)
        wait_layout(proc, preferences, predicate=lambda layout: "settings" not in layout)
        restored = tomllib.loads(preferences.read_text())
        for section in ("appearance", "audio", "typing", "bindings", "cursor", "presentation"):
            assert restored[section] == baseline[section], f"Native Settings did not restore {section}"
        assert tomllib.loads(config.read_text()) == profiles, "Native Settings did not restore original private profiles"
        assert database.execute(buffer_query).fetchall() == original_buffers, "Settings proof changed original buffer drafts"
        actual_history = database.execute(history_query).fetchall()
        buffer_column, id_column = history_columns.index("buffer"), history_columns.index("id")
        new_keys = {(row[0], row[1]) for row in new_rows}
        assert len(new_rows) == 2 and len(new_keys) == 2, new_rows
        assert len(actual_history) == len(history_before) + len(new_rows)
        assert [row for row in actual_history if (row[buffer_column], row[id_column]) not in new_keys] == history_before, "Settings proof changed old non-presence history"
        actual_new = database.execute(
            "SELECT buffer,id,nick,text,own,pending,deleted FROM messages WHERE text IN (?,?) ORDER BY text",
            (incoming_text, outgoing_text),
        ).fetchall()
        assert actual_new == sorted(new_rows, key=lambda row: row[3]), lab._redact(repr(actual_new))
        print("Native Settings evidence:", json.dumps({
            "wide_and_compact_categories": list(categories), "shared_identity_networks": ["Local", "Other"],
            "failed_save_kept_connection": True, "successful_reconnect": lifecycle,
            "restored_profiles_and_preferences": True, "intentional_new_rows": new_rows,
        }, ensure_ascii=False), flush=True)
        return new_rows
    finally:
        database.close()






def select_native_buffer(proc, window, preferences, database_path, args, buffer_id):
    before = save_state(proc, preferences, database_path)
    if before[1] != (buffer_id,):
        network, target = buffer_id.split("/", 1)
        switch_buffer(target, network)
    after = save_state(proc, preferences, database_path)
    assert after[1] == (buffer_id,), (buffer_id, after)
    layout = read_layout(preferences)
    assert layout["active_buffer"] == buffer_id, (buffer_id, layout)
    return layout


def open_avy(window, preferences, destination, label):
    binding = tomllib.loads(preferences.read_text())["bindings"]["jump-channel"]
    chords = tuple(stroke.replace("C-", "ctrl+").replace("M-", "alt+").replace("S-", "shift+")
                   for stroke in binding.split())
    framed_keys(window, *chords)
    SESSION.until("selected-server Avy owns non-text focus", lambda: native_input_state(window) is None)
    return capture_window(window, destination, "avy-" + label)


def choose_avy_buffer(proc, window, preferences, database_path, args, buffer_id, keys, destination, label):
    before = save_state(proc, preferences, database_path)
    assert before[0]["selected_server"] == buffer_id.split("/", 1)[0], before
    open_avy(window, preferences, destination, label + "-hints")
    framed_keys(window, *tuple(keys))
    selected = save_state(proc, preferences, database_path)
    assert selected[1] == (buffer_id,), (buffer_id, keys, selected)
    capture_window(window, destination, "avy-" + label + "-selected")
    return read_layout(preferences)


def probe_avy_cancel(proc, window, preferences, database_path, args, destination, *,
                     lab, cleanup, own_nick):
    """Cancel on the original Input; isolate destructive compose controls in a real query."""
    active = save_state(proc, preferences, database_path)[1][0]
    wait_read_marker(proc, database_path, active)
    baseline = save_state(proc, preferences, database_path)
    original_settings = tomllib.loads(preferences.read_text())
    initial = native_input_state(window)
    assert initial is not None and initial["caret"] is not None, initial
    captures = []
    for key_name, label in (("Escape", "escape"), ("ctrl+g", "c-g"), ("1", "invalid")):
        captures.append(str(open_avy(window, preferences, destination, label + "-hints")))
        framed_keys(window, key_name)
        restored = _composer_native(window, initial["text"], initial["cursor"], initial["anchor"])
        assert restored == initial, ("Avy cancellation changed native editing state", label, initial, restored)
        assert_same_state("Avy " + label + " preserves buffer/draft/read/panel intent",
                          baseline, save_state(proc, preferences, database_path))
        captures.append(str(capture_window(window, destination, "avy-" + label + "-restored")))
    assert (initial["text"], initial["cursor"], initial["anchor"]) == ("", 0, 0), initial
    database = sqlite3.connect(f"file:{database_path}?mode=ro", uri=True)
    try:
        original_buffers = database.execute("SELECT * FROM buffers ORDER BY id").fetchall()
        original_history = database.execute(
            "SELECT * FROM messages WHERE presence=0 ORDER BY seq").fetchall()
    finally:
        database.close()
    nonce = os.urandom(4).hex()
    nick = "avycontrol" + nonce
    control_id = "Local/" + nick
    seed = "Avy isolated compose control " + nonce
    seed_row = (control_id, nick, seed, 0, 0)
    peer = fixtures.BootstrapClient(
        dict(lab.endpoints["direct"], nick=nick, username=nick), lab.ca_file,
        authenticate=False)
    stopped = threading.Event()
    reader_errors = []

    def keepalive():
        try:
            while not stopped.is_set():
                peer.read(time.monotonic() + 86400)
        except Exception as error:
            if not stopped.is_set():
                reader_errors.append(error)

    reader = threading.Thread(target=keepalive, name="avy-control-irc")

    def close_peer():
        stopped.set()
        try:
            peer.close()  # Socket shutdown unblocks the real TLS reader before stream close.
        finally:
            if reader.ident is not None:
                reader.join()

    cleanup.callback(close_peer)
    reader.start()
    peer.send(f"PRIVMSG {own_nick} :{seed}")
    wait_messages(proc, database_path, [seed_row])
    select_native_buffer(proc, window, preferences, database_path, args, control_id)
    wait_read_marker(proc, database_path, control_id)
    control_baseline = save_state(proc, preferences, database_path)
    control_initial = _composer_native(window, "", 0, 0)
    assert not reader_errors and reader.is_alive(), reader_errors
    captures.append(str(open_avy(window, preferences, destination, "dead-acute-hints")))
    POINTER.publish_keymap()
    path = GUI_DEBUG[window][1]
    marker = path.stat().st_size
    key_event = lambda code: re.compile(
        rf"wl_keyboard[@#]\d+\.key\(\d+,\s*\d+,\s*{code},\s*1\)")
    dead = POINTER.keyboard_key("dead_acute")
    await_native_frame(window, marker, key_event(dead))
    _composer_native(window, "", 0, 0)
    offset = path.stat().st_size
    letter = POINTER.keyboard_key("e")
    await_native_frame(window, offset, key_event(letter))
    observe_native_input(window, destination, "avy-dead-acute-plain-e", "e", 1, 1)
    offset = path.stat().st_size
    backspace = POINTER.keyboard_key("BackSpace")
    await_native_frame(window, offset, key_event(backspace))
    _composer_native(window, "", 0, 0)
    # Positive control: the same keyboard/map must genuinely compose outside Avy.
    POINTER.keyboard_key("dead_acute")
    offset = path.stat().st_size
    POINTER.keyboard_key("e")
    await_native_frame(window, offset, key_event(letter))
    observe_native_input(window, destination, "avy-compose-positive-eacute", "é", 2, 2)
    trace = path.read_bytes()[marker:].decode(errors="replace")
    first = key_event(dead).search(trace)
    last = list(key_event(letter).finditer(trace))[-1]
    assert first is not None and not re.search(
        r"wl_keyboard[@#]\d+\.keymap\(", trace[first.end():last.end()]), (
            "A keymap reset invalidated the dead-key isolation comparison", trace)
    offset = path.stat().st_size
    POINTER.keyboard_key("BackSpace")
    await_native_frame(window, offset, key_event(backspace))
    _composer_native(window, "", 0, 0)
    assert _composer_native(window, "", 0, 0) == control_initial
    assert_same_state("Avy dead compose key does not leak into the control composer",
                      control_baseline, save_state(proc, preferences, database_path))
    assert not reader_errors and reader.is_alive(), reader_errors
    select_native_buffer(proc, window, preferences, database_path, args, active)
    restored = _composer_native(window, initial["text"], initial["cursor"], initial["anchor"])
    assert restored == initial, ("Compose controls changed the original native Input", initial, restored)
    returned = save_state(proc, preferences, database_path)
    assert_same_state("Avy controls preserve all original buffer/draft/read/panel intent", baseline,
                      (returned[0], returned[1], [row for row in returned[2] if row[0] != control_id]))
    assert saved_workspace_intent(tomllib.loads(preferences.read_text())) == saved_workspace_intent(
        original_settings)
    wait_rows(proc, database_path, "SELECT * FROM buffers WHERE id<>? ORDER BY id",
              (control_id,), original_buffers)
    wait_rows(proc, database_path,
              "SELECT * FROM messages WHERE presence=0 AND buffer<>? ORDER BY seq",
              (control_id,), original_history)
    wait_rows(proc, database_path,
              "SELECT buffer,nick,text,own,pending,deleted,presence FROM messages WHERE buffer=?",
              (control_id,), [seed_row + (0, 0)])
    assert not reader_errors and reader.is_alive(), reader_errors
    evidence = destination.with_name(destination.stem + "-avy-cancel-evidence.json")
    evidence.write_text(json.dumps({"captures": captures, "control_seed": seed_row},
                                   indent=2, ensure_ascii=False) + "\n")
    return [seed_row]


def saved_workspace_intent(settings, remaps=None):
    remaps = remaps or {}
    return {**settings,
            "presentation": {**settings["presentation"], "irc_channels": sorted(
                remaps.get(value, value) for value in settings["presentation"]["irc_channels"])},
            "presence": {**settings["presence"], "channels": {
                remaps.get(key, key): value for key, value in settings["presence"]["channels"].items()}},
            "workspace": {
                key: remaps.get(value, value) if isinstance(value, str) else value
                for key, value in semantic_workspace(settings["workspace"]).items()}}


def rename_observer_query(observer, previous, target, lab):
    observer.send("NICK " + target)
    acknowledged = receive_until(observer, "NICK", [target], previous, redact=lab._redact)
    print("Actual TLS observer NICK acknowledgment:", lab._redact(repr(acknowledged)), flush=True)


def probe_flat_refresh(proc, window, preferences, database_path, args, destination, *,
                       observer, query_target, query_id, other_query_id, cleanup, lab):
    """Preserve per-buffer native draft/undo/read state through global selection and NICK."""
    baseline = save_state(proc, preferences, database_path)
    assert baseline[1] == (query_id,), baseline
    assert_workspace({"workspace": baseline[0]}, query_id, baseline[0]["people_mode"])
    previous_other = baseline[0].get("last_buffer:Other")
    bounds = window_geometry(window)
    query_ids = (query_id, other_query_id)
    buffer_query = "SELECT id,server,target,kind,draft FROM buffers WHERE id IN (?,?) ORDER BY id"
    history_query = (
        "SELECT * FROM messages WHERE buffer IN (?,?) AND presence=0 "
        "ORDER BY buffer,sort_time,seq")
    database = sqlite3.connect(f"file:{database_path}?mode=ro", uri=True)
    try:
        original_buffers = database.execute(buffer_query, query_ids).fetchall()
        assert [row[0] for row in original_buffers] == sorted(query_ids), original_buffers
        drafts = {row[0]: row[4] for row in original_buffers}
        original_draft = drafts[query_id]
        end = len(original_draft.encode("utf-8"))
        history = database.execute(history_query, query_ids)
        buffer_column = [column[0] for column in history.description].index("buffer")
        original_history = history.fetchall()
        assert all(any(row[buffer_column] == identity for row in original_history)
                   for identity in query_ids), original_history
        current_drafts = dict(drafts)

        def draft_state(label, identity):
            state = save_state(proc, preferences, database_path)
            assert state[1] == (identity,), state
            assert_workspace({"workspace": state[0]}, identity, baseline[0]["people_mode"],
                             server=identity.split("/", 1)[0])
            expected = [(buffer, unread, marker, current_drafts.get(buffer, draft))
                        for buffer, unread, marker, draft in baseline[2]]
            assert state[2] == expected, (label, expected, state[2])
            layout = read_layout(preferences)
            assert layout["active_buffer"] == identity, layout
            text = current_drafts[identity]
            end = len(text.encode("utf-8"))
            action = command("end-of-buffer", window=window)
            return observe_native_input(window, destination, label, text, end, end,
                                        after_action=action)

        select_native_buffer(proc, window, preferences, database_path, args, CHANNEL_ID)
        assert read_layout(preferences)["conversation"]["buffer"] == CHANNEL_ID
        select_native_buffer(proc, window, preferences, database_path, args, query_id)
        assert_same_state("Global picker restores the original selected conversation", baseline,
                          save_state(proc, preferences, database_path))
        for identity, label in ((query_id, "local"), (other_query_id, "other")):
            select_native_buffer(proc, window, preferences, database_path, args, identity)
            draft_state("refresh-" + label + "-original", identity)
            suffix = " [astra " + label + " buffer undo]"
            seed_clipboard(suffix, cleanup)
            framed_keys(window, "ctrl+v")
            current_drafts[identity] += suffix
            pasted_end = len(current_drafts[identity].encode("utf-8"))
            _composer_native(window, current_drafts[identity], pasted_end, pasted_end)
            draft_state("refresh-" + label + "-pasted", identity)
        for identity, label in ((query_id, "local"), (other_query_id, "other")):
            select_native_buffer(proc, window, preferences, database_path, args, identity)
            draft_state("refresh-" + label + "-reselected", identity)
            command("undo", window=window)
            current_drafts[identity] = drafts[identity]
            restored_end = len(current_drafts[identity].encode("utf-8"))
            _composer_native(window, current_drafts[identity], restored_end, restored_end)
            draft_state("refresh-" + label + "-independent-undo", identity)
        if previous_other and previous_other != other_query_id:
            select_native_buffer(proc, window, preferences, database_path, args, previous_other)
        select_native_buffer(proc, window, preferences, database_path, args, query_id)
        assert_same_state("Global selection preserves per-buffer drafts, reads and selected server", baseline,
                          save_state(proc, preferences, database_path))
        assert database.execute(history_query, query_ids).fetchall() == original_history
        capture_window(window, destination, "refresh-global-selection-restored")
        normal = tomllib.loads(preferences.read_text())
        assert_workspace(normal, query_id, baseline[0]["people_mode"])
        normal_action = command("end-of-buffer", window=window)
        normal_input = observe_native_input(
            window, destination, "refresh-before-rename", original_draft, end, end,
            after_action=normal_action)

        observer.send("VERSION")
        _, support, _ = observer.wait(
            lambda actual, params, prefix: actual == "005" and
            any(re.fullmatch(r"NICKLEN=\d+", value) for value in params), timeout=15)
        nick_limits = [int(value.split("=", 1)[1]) for value in support
                       if re.fullmatch(r"NICKLEN=\d+", value)]
        renamed = query_target + "astra"
        assert len(nick_limits) == 1 and len(renamed.encode("utf-8")) <= nick_limits[0], support
        renamed_ids = tuple(identity.rsplit("/", 1)[0] + "/" + renamed for identity in query_ids)
        assert database.execute(buffer_query, renamed_ids).fetchall() == [], renamed_ids
        suffix = " [astra shared rename undo]"
        seed_clipboard(suffix, cleanup)
        framed_keys(window, "ctrl+v")
        save_state(proc, preferences, database_path)
        appended = original_draft + suffix
        wait_rows(proc, database_path, "SELECT draft FROM buffers WHERE id=?",
                  (query_id,), [(appended,)])
        observe_native_input(
            window, destination, "refresh-paste-before-rename", appended,
            len(appended.encode("utf-8")), len(appended.encode("utf-8")))
        current_draft = appended
        for previous, target, old_ids, current_ids, label in (
                (query_target, renamed, query_ids, renamed_ids, "renamed"),
                (renamed, query_target, renamed_ids, query_ids, "original-name")):
            rename_observer_query(observer, previous, target, lab)
            remaps = dict(zip(query_ids, current_ids))
            expected_buffers = sorted(
                (remaps[identity], server, target, kind,
                 current_draft if identity == query_id else draft)
                for identity, server, _, kind, draft in original_buffers)
            wait_rows(proc, database_path, buffer_query, current_ids, expected_buffers)
            save_state(proc, preferences, database_path)
            saved = tomllib.loads(preferences.read_text())
            assert saved_workspace_intent(saved) == saved_workspace_intent(normal, remaps), (
                normal, saved)
            assert saved["workspace"]["visible_buffers"].splitlines() == [current_ids[0]], saved
            assert database.execute(buffer_query, old_ids).fetchall() == [], old_ids
            assert database.execute(
                "SELECT buffer FROM messages WHERE buffer IN (?,?)", old_ids).fetchall() == [], old_ids
            expected_history = [
                tuple(remaps.get(value, value) if index == buffer_column else value
                      for index, value in enumerate(row)) for row in original_history]
            actual_history = database.execute(history_query, current_ids).fetchall()
            assert actual_history == expected_history, (expected_history, actual_history)
            assert_saved_buffers(database_path, expected_buffers, current_ids[0])
            current_end = len(current_draft.encode("utf-8"))
            observe_native_input(
                window, destination, "refresh-" + label,
                current_draft, current_end, current_end)
            assert window_geometry(window) == bounds, (bounds, window_geometry(window))
            if target == renamed:
                command("undo", window=window)
                save_state(proc, preferences, database_path)
                wait_rows(proc, database_path, "SELECT draft FROM buffers WHERE id=?",
                          (current_ids[0],), [(original_draft,)])
                undone = tomllib.loads(preferences.read_text())
                assert saved_workspace_intent(undone) == saved_workspace_intent(saved), (saved, undone)
                observe_native_input(
                    window, destination, "refresh-renamed-undo",
                    original_draft, end, end)
                current_draft = original_draft
        restore_action = command("end-of-buffer", window=window)
        save_state(proc, preferences, database_path)
        restored = tomllib.loads(preferences.read_text())
        assert saved_workspace_intent(restored) == saved_workspace_intent(normal), (normal, restored)
        assert_saved_buffers(database_path, original_buffers, query_id)
        assert database.execute(history_query, query_ids).fetchall() == original_history
        restored_input = observe_native_input(
            window, destination, "refresh-query-restored", original_draft, end, end,
            expected=normal_input, after_action=restore_action)
        assert restored_input == normal_input and window_geometry(window) == bounds, (
            normal_input, restored_input, bounds, window_geometry(window))
    finally:
        database.close()
    print("Global selection and real dual-query NICK/undo preserve independent drafts, reads and history: passed",
          flush=True)


def _composer_native(window, text, cursor, anchor):
    """Wait for actual compositor-facing UTF-8 BYTE offsets and an onscreen caret."""
    result = None

    def observed():
        nonlocal result
        state = native_input_state(window)
        if (state is None or state["caret"] is None or
                (state["text"], state["cursor"], state["anchor"]) != (text, cursor, anchor)):
            return False
        x, y, width, height = state["caret"]
        geometry = window_geometry(window)
        assert width > 0 and height > 0 and x >= 0 and y >= 0, state
        assert x + width <= geometry["WIDTH"] and y + height <= geometry["HEIGHT"], (state, geometry)
        result = dict(state)
        return result

    SESSION.until("exact native composer committed text, byte selection and visible caret", observed)
    return result


def _composer_popup_option(window, popup, index):
    navigation = ("Home",) + ("Down",) * index
    marker = GUI_DEBUG[window][1].stat().st_size
    key(*navigation)
    await_native_frame(window, marker,
                       re.compile(r"wl_keyboard[@#]\d+\.key\(\d+,\s*\d+,\s*\d+,\s*1\)"),
                       len(navigation), surface=int(popup["surface"]))
    popup_keys(window, popup, "Return")


def native_reply_excerpt_region(window, preferences, destination, label):
    """Locate the native excerpt from its actual rendered heading/glyph extent."""
    import csv
    import io
    caret = native_input_rectangle(window)
    assert caret is not None, "Reply proof requires the actual focused native Input"
    x, y, width, _ = read_layout(preferences)["conversation"]["rect"]
    image = capture_window(window, destination, label + "-heading")
    result = subprocess.run(["tesseract", str(image), "stdout", "--psm", "11", "tsv"],
                            check=True, capture_output=True, text=True, timeout=20)
    headings = []
    for row in csv.DictReader(io.StringIO(result.stdout), delimiter="\t"):
        if row.get("text", "").casefold() != "replying":
            continue
        left, top, glyph_width, glyph_height = (int(row[key]) for key in ("left", "top", "width", "height"))
        if x <= left < x + width and y <= top < top + glyph_height < caret[1]:
            headings.append((left, top, left + glyph_width, top + glyph_height))
    assert headings, ("No actual Replying heading above native Input", caret, result.stdout)
    heading = max(headings, key=lambda box: box[3])
    top = heading[3] + 1
    bottom = min(caret[1], heading[3] + 2 * (heading[3] - heading[1]))
    assert top < bottom, (heading, caret)
    return (x, top, width, bottom - top)


def begin_native_reply(window, layout, zoom, destination, *, label="loaded-reply", search=False,
                       expected_copy=None):
    """Choose Reply=1 on the actual unique 19A7D3 incoming message-body swatch.

    The caller first checks the real wire policy and SQLite referenceable row.
    Default: wait for the freshly received marker to paint, without scrolling.
    search=True: re-find this same loaded row using bounded REAL upward wheel
    input after the probe's own six sends. No search model or message-ID hook.
    Each search wheel waits for a changed-then-stable real transcript RGB crop
    after its owned Root paint; the native Composer/caret is outside that crop.
    Return to the unchanged native Input after capturing the visible Cancel reply
    focus state, so the caller can type or reuse this for compact captures.
    """
    before = native_input_state(window)
    assert before is not None and before["caret"] is not None, before
    expected = (before["text"], before["cursor"], before["anchor"])
    rectangle = layout["conversation"]["rect"]
    rgb = (25, 167, 211)
    previous = None
    search_evidence = None
    marker_image = None
    marker_frame = None

    def marker():
        nonlocal previous, marker_image, marker_frame
        start = GUI_DEBUG[window][1].stat().st_size if search else None
        marker_image = native_rgb_capture(window, capture_timing=search)
        if search:
            marker_frame = {"step": step, "log_start": start,
                            "log_end": GUI_DEBUG[window][1].stat().st_size,
                            "frames": marker_image["capture_timestamps"],
                            "rgb_sha256": hashlib.sha256(marker_image["pixels"]).hexdigest()}
        components = [component for component in native_marker_components(marker_image, rectangle, rgb)
                      if component["full"]]
        # Reveal can temporarily split the swatch into multiple complete tiles.
        # Only a unique candidate may converge; final ambiguity times out.
        if len(components) != 1:
            previous = None
            return False
        component = components[0]
        stable = component["bbox"] == previous
        previous = component["bbox"]
        return component if stable else False

    if search:
        metrics = pane_metrics(layout, zoom)
        top = metrics["y"] + metrics["top"]
        bottom = before["caret"][1] - before["caret"][3]
        assert top < bottom, (metrics, before)
        x = metrics["x"] + metrics["left"] + metrics["inner_width"] / 2
        y = (top + bottom) / 2
        from PIL import Image

        debug = GUI_DEBUG[window][1]
        geometry = window_geometry(window)
        # Keep the RGB sample inside the transcript's content columns and above
        # the native Input's first-line caret guard, never its blinking caret.
        crop = (math.ceil(metrics["x"] + metrics["left"]), math.ceil(top),
                math.floor(metrics["x"] + metrics["left"] + metrics["inner_width"]),
                math.floor(bottom - metrics["padding_y"] - metrics["gap"]))
        assert 0 <= crop[0] < crop[2] <= geometry["WIDTH"], (crop, geometry)
        assert 0 <= crop[1] < crop[3] <= geometry["HEIGHT"], (crop, geometry)
        log_base = debug.stat().st_size
        log_capture = destination.with_name(destination.stem + "-" + label + "-wheel-settle.wayland.log")
        search_evidence = {"viewport_crop": crop, "geometry": geometry,
                           "debug_log": str(log_capture), "log_base_offset": log_base,
                           "log_offset_mapping": "artifact byte = recorded offset - log_base_offset",
                           "root_surface": GUI_DEBUG[window][2], "steps": []}
        evidence_path = destination.with_name(destination.stem + "-" + label + "-wheel-settle.json")

        def viewport_sample():
            start = debug.stat().st_size
            image = native_rgb_capture(window, capture_timing=True)
            assert image["geometry"] == geometry, "Reply search changed native window geometry"
            assert len(image["capture_timestamps"]) == 1, (
                "Reply settle requires one actual screencopy presentation frame",
                image["capture_timestamps"])
            pixels = b"".join(image["pixels"][(row * image["width"] + crop[0]) * 3:
                                             (row * image["width"] + crop[2]) * 3]
                              for row in range(crop[1], crop[3]))
            return pixels, {"log_start": start, "log_end": debug.stat().st_size,
                            "frames": image["capture_timestamps"],
                            "rgb_sha256": hashlib.sha256(pixels).hexdigest()}

        step = 0
        found = None
        try:
            found = marker()
            if not found and previous is not None:
                found = marker()
            # ponytail: this seed is only six new own rows back; bounded native
            # wheel traversal suffices, not a general history-search facility.
            for step in range(1, 26):
                if found:
                    break
                move_pointer(window, x, y)
                baseline, baseline_frame = viewport_sample()
                offset = debug.stat().st_size
                record = {"step": step, "axis_log_start": offset,
                          "baseline": baseline_frame, "samples": []}
                search_evidence["steps"].append(record)
                POINTER.scroll(-1)
                await_native_frame(window, offset,
                                   re.compile(r"wl_pointer[@#]\d+\.axis\(\d+,\s*0,"))
                record["owned_root_paint_log_end"] = debug.stat().st_size
                changed = False
                last_pixels = None
                last_timestamp = baseline_frame["frames"][0]["timestamp_ns"]

                def settled():
                    nonlocal changed, last_pixels, last_timestamp
                    pixels, frame = viewport_sample()
                    timestamp = frame["frames"][0]["timestamp_ns"]
                    fresh = timestamp > last_timestamp
                    if fresh:
                        changed = changed or pixels != baseline
                    stable = fresh and changed and pixels == last_pixels
                    frame.update(sample=len(record["samples"]), fresh_frame=fresh,
                                 changed=changed, stable=stable)
                    record["samples"].append(frame)
                    if fresh:
                        last_pixels = pixels
                        last_timestamp = timestamp
                    return stable

                SESSION.until("changed then stable actual reply transcript viewport RGB", settled)
                found = marker()
                if not found and previous is not None:
                    found = marker()
            assert found, "Real loaded reply marker was not found in 25 upward wheel steps"
            found_capture = destination.with_name(destination.stem + "-" + label + "-found-frame.png")
            Image.frombytes("RGB", (marker_image["width"], marker_image["height"]),
                            marker_image["pixels"]).save(found_capture)
            search_evidence.update(found=found, found_frame=marker_frame,
                                   found_capture=str(found_capture))
        finally:
            log_end = debug.stat().st_size
            with debug.open("rb") as source:
                source.seek(log_base)
                log_capture.write_bytes(source.read(log_end - log_base))
            search_evidence["log_end_offset"] = log_end
            evidence_path.write_text(json.dumps(search_evidence, indent=2) + "\n")
            print("Native reply wheel paint-settle evidence:", evidence_path, flush=True)
    else:
        found = SESSION.until("one stable complete actual incoming reply swatch", marker)
    left, top, right, bottom = found["bbox"]

    point = ((left + right) / 2, (top + bottom) / 2)
    def right_click():
        move_pointer(window, *point)
        POINTER.button(True, code=0x111)
        POINTER.button(False, code=0x111)

    popup = open_native_popup(window, right_click)
    bounds_evidence = pointer_popup_bounds(window, popup, point)
    if expected_copy is not None:
        _composer_popup_option(window, popup, 0)
        SESSION.until("message Copy text owns exact raw IRC text on clipboard",
                      lambda: clipboard_text() == expected_copy)
        popup = open_native_popup(window, right_click)
        bounds_evidence = pointer_popup_bounds(window, popup, point)
    menu_capture = capture_window(window, destination, label + "-message-context")
    _composer_popup_option(window, popup, 1)
    resolved = _composer_native(window, *expected)
    reply_capture = capture_window(window, destination, label + "-resolved-context")
    framed_keys(window, "shift+Tab")
    SESSION.until("native Cancel reply focus leaves Input", lambda: native_input_state(window) is None)
    cancel_capture = capture_window(window, destination, label + "-cancel-visible-focused")
    framed_keys(window, "Tab")
    _composer_native(window, *expected)
    result = {"marker": found, "popup": popup, "pointer_bounds": bounds_evidence, "native": resolved,
              "captures": [str(menu_capture), str(reply_capture), str(cancel_capture)]}
    if search:
        result.update(search=search_evidence, search_evidence=str(evidence_path))
        result["captures"].append(str(found_capture))
    return result


def cancel_native_reply(window, destination, *, label="reply-cancel", expected_text=None):
    """Activate the actual visible Cancel reply button, not C-g or a model setter."""
    before = native_input_state(window)
    assert before is not None and before["caret"] is not None, before
    if expected_text is not None:
        assert before["text"] == expected_text, (expected_text, before)
    expected = (before["text"], before["cursor"], before["anchor"])
    framed_keys(window, "shift+Tab")
    SESSION.until("native Cancel reply button focus", lambda: native_input_state(window) is None)
    focused = capture_window(window, destination, label + "-visible-button")
    framed_keys(window, "Return")
    after = _composer_native(window, *expected)
    cancelled = capture_window(window, destination, label + "-cancelled-draft-retained")
    return {"before": before, "after": after,
            "captures": [str(focused), str(cancelled)]}


def probe_reply_paste(proc, window, prefs, db, args, dest, observer, *, query_id,
                      query_target, own_nick, cleanup, lab, receive):
    """Return EXACT known new (buffer,nick,text,own,pending) nonpresence rows.

    Entry: query_id is the visible conversation, EMPTY draft, no reply/pending paste.
    Parent inserts this after its ordinary Bold send and before pending-echo work.
    Exit: same workspace/query, EMPTY draft, no reply/pending paste; every old
    message is byte-for-byte unchanged. No messages are removed.

    With real reply policy and a server-assigned referenceable seed ID, return
    eight rows: one incoming seed + five literal bulk sends + preserved-reply
    original draft + visibly cancelled-reply draft. Otherwise report the exact
    unavailable reply prerequisite, still exercise ALL bulk paths, and return
    seven rows (seed + five bulk + original draft without a reply). Never call an
    unavailable reply path passed, and never omit the bulk probe for that reason.
    """
    assert proc.poll() is None, proc.returncode
    assert hasattr(observer, "frames") and hasattr(observer, "last_tags"), "Real WireObserver required"
    _composer_native(window, "", 0, 0)
    saved = save_state(proc, prefs, db)
    assert saved[1] == (query_id,), saved
    assert_workspace({"workspace": saved[0]}, query_id, saved[0]["people_mode"])
    layout = read_layout(prefs)
    assert layout["active_buffer"] == query_id, layout
    wait_rows(proc, db, "SELECT draft FROM buffers WHERE id=?", (query_id,), [("",)])
    bounds = window_geometry(window)
    assert not native_marker_components(native_rgb_capture(window), layout["conversation"]["rect"],
                                        (25, 167, 211)), "19A7D3 reply swatch was already present"

    def wire_barrier(label):
        token = "astra-composer-" + label + "-" + os.urandom(6).hex()
        observer.send("PING :" + token)
        observer.wait(lambda command, params, prefix:
                      command == "PONG" and token in params, timeout=15)

    # The ordinary observer can be registered without CAP. Negotiate advertised
    # capabilities on THAT real connection instead of treating an empty ACK set
    # as proof the daemon lacks replies. No invented reply capability exists.
    observer.send("CAP LS 302")
    available = set()
    while True:
        _, params, _ = observer.wait(lambda command, params, prefix:
                                     command == "CAP" and "LS" in params, timeout=15)
        available.update(cap.split("=", 1)[0] for cap in params[-1].split())
        if len(params) < 4 or params[-2] != "*":
            break
    requested = [name for name in ("message-tags", "server-time", "echo-message")
                 if name in available and name not in observer.capabilities]
    if requested:
        observer.send("CAP REQ :" + " ".join(requested))
        observer.wait(lambda command, params, prefix:
                      command == "CAP" and ("ACK" in params or "NAK" in params), timeout=15)
    wire_barrier("checkpoint")
    wire_start = len(observer.frames)
    database = sqlite3.connect(f"file:{db}?mode=ro", uri=True)
    try:
        frontier = database.execute("SELECT COALESCE(MAX(seq),0) FROM messages").fetchone()[0]
        old_query = "SELECT * FROM messages WHERE seq<=? ORDER BY seq"
        old_history = database.execute(old_query, (frontier,)).fetchall()
        new_query = ("SELECT buffer,nick,text,own,pending FROM messages "
                     "WHERE seq>? AND presence=0 ORDER BY buffer,nick,text,own,pending")
        expected = []
        expected_wire = []
        reply_tag_names = ("+reply", "+draft/reply", "reply", "draft/reply")
        evidence = {"capabilities_ack": sorted(observer.capabilities),
                    "advertised": sorted(available),
                    "clienttagdeny": observer.isupport.get("CLIENTTAGDENY"),
                    "captures": [], "native_states": {}}

        def unchanged_document(state):
            return _composer_native(window, state["text"], state["cursor"], state["anchor"])

        def exact_rows():
            wait_rows(proc, db, new_query, (frontier,), sorted(expected))
            assert database.execute(old_query, (frontier,)).fetchall() == old_history, (
                "Composer probe mutated or deleted old history")

        def own_payload_frames():
            # Typing TAGMSG/read-marker traffic is metadata, not a payload send.
            # Also reject command mutations: a literal /quit must not become QUIT.
            return [frame for frame in observer.frames[wire_start:]
                    if frame["prefix"].split("!", 1)[0] == own_nick and frame["command"] in
                    ("PRIVMSG", "NOTICE", "QUIT", "PART", "JOIN", "NICK")]

        def exact_wire(label):
            wire_barrier(label)
            actual = []
            for frame in own_payload_frames():
                reply_tags = {name: frame["tags"][name] for name in reply_tag_names
                              if name in frame["tags"]}
                actual.append((frame["command"], frame["params"], reply_tags))
            assert actual == expected_wire, lab._redact(
                f"Exact own wire sequence differs: expected {expected_wire!r}, got {actual!r}")

        def receive_own(text, reply=""):
            receive("PRIVMSG", [query_target, text], own_nick)
            frame = observer.frames[-1]
            assert (frame["command"], frame["params"], frame["prefix"].split("!", 1)[0]) == (
                "PRIVMSG", [query_target, text], own_nick), frame
            assert observer.last_tags == frame["tags"], "Observer last_tags is not the actual matched frame"
            tags = {name: frame["tags"][name] for name in reply_tag_names if name in frame["tags"]}
            wanted = {"+reply": reply} if reply else {}
            assert tags == wanted, (text, wanted, tags)
            expected_wire.append(("PRIVMSG", [query_target, text], wanted))
            expected.append((query_id, own_nick, text, 1, 0))

        nonce = os.urandom(5).hex()
        seed_text = (f"Astra loaded reply {nonce}: "
                     f"\x0419A7D3,19A7D3Reply{nonce}\x0f real incoming context.")
        seed_line = f"PRIVMSG {own_nick} :{seed_text}"
        assert len(seed_line.encode("utf-8")) <= 510, "Seed exceeds ordinary IRC framing limit"
        observer.send(seed_line)
        seed_echo = None
        if "echo-message" in observer.capabilities:
            receive("PRIVMSG", [own_nick, seed_text], query_target)
            seed_echo = dict(observer.last_tags)
        expected.append((query_id, query_target, seed_text, 0, 0))
        exact_rows()
        seed = database.execute(
            "SELECT seq,id,time,reply,pending,deleted,presence FROM messages "
            "WHERE seq>? AND buffer=? AND nick=? AND text=? AND own=0",
            (frontier, query_id, query_target, seed_text)).fetchall()
        assert len(seed) == 1, seed
        sequence, seed_id, seed_time, seed_reply, pending, deleted, presence = seed[0]
        assert (seed_reply, pending, deleted, presence) == ("", 0, 0, 0), seed
        if seed_echo is not None:
            if "msgid" in seed_echo:
                assert seed_id == seed_echo["msgid"], (seed, seed_echo)
            if "time" in seed_echo:
                assert seed_time == seed_echo["time"], (seed, seed_echo)
        supported = client_tags_allowed(observer, "reply") and bool(seed_id) and not seed_id.startswith("local:")
        reason = None
        if "message-tags" not in observer.capabilities:
            reason = "message-tags was not ACKed on the actual TLS observer"
        elif not client_tags_allowed(observer, "reply"):
            reason = "actual CLIENTTAGDENY does not permit reply"
        elif not seed_id or seed_id.startswith("local:"):
            reason = "actual incoming SQLite row has no server-assigned referenceable msgid"
        evidence["reply"] = {"supported": supported, "unavailable": reason,
                             "seed": {"seq": sequence, "id": seed_id, "time": seed_time,
                                      "buffer": query_id, "nick": query_target, "text": seed_text},
                             "observer_echo_tags": seed_echo}
        if supported:
            reply = begin_native_reply(window, layout, args.zoom, dest, label="reply-loaded",
                                       expected_copy=seed_text)
            evidence["captures"].extend(reply["captures"])
        else:
            print("Native reply component UNAVAILABLE (bulk still required): " + reason, flush=True)

        original = "Keep this draft"
        type_text(original)
        length = len(original.encode("utf-8"))
        _composer_native(window, original, length, length)
        # Establish an ACTUAL first-line caret, before any asynchronous paste or
        # selection. Home/End here are native keys, not the command-palette locator.
        framed_keys(window, "ctrl+Home")
        first = _composer_native(window, original, 0, 0)
        framed_keys(window, "ctrl+End")
        end = _composer_native(window, original, length, length)
        assert first["caret"][1] == end["caret"][1], (
            "Controlled original draft is not one native shaped line", first, end)
        save_state(proc, prefs, db)  # BEFORE clipboard read/selection.
        wait_rows(proc, db, "SELECT draft FROM buffers WHERE id=?", (query_id,), [(original,)])
        # A Core save may precede the next native layout.
        # Bind this baseline to the following command's completed, painted state.
        original_action = command("end-of-buffer", window=window)
        original_state = observe_native_input(window, dest, "reply-saved-original-caret",
                                              original, length, length, after_action=original_action)
        evidence["native_states"]["original_saved"] = original_state
        payload = "\x02one\x02\r\n\r\n/quit\r\n猫😀\r\n \r\nlast\r\n"
        normalized = "\x02one\x02\n\n/quit\n猫😀\n \nlast\n"
        separate = ["\x02one\x02", "/quit", "猫😀", " ", "last"]
        assert payload.replace("\r\n", "\n") == normalized
        assert normalized.count("\n") + 1 == 7 and [line for line in normalized.split("\n") if line] == separate
        assert len(normalized.encode("utf-8")) < 64 * 1024
        seed_clipboard(payload, cleanup)

        def confirm_paste(state, label, index):
            assert window_geometry(window) == bounds, "Window changed inside clipboard transaction"
            offset = GUI_DEBUG[window][1].stat().st_size
            key("ctrl+v")
            expanded = None

            def ready():
                nonlocal expanded
                actual = native_input_state(window)
                if (actual is None or actual["caret"] is None or
                        (actual["text"], actual["cursor"], actual["anchor"]) !=
                        (state["text"], state["cursor"], state["anchor"])):
                    return False
                if actual["caret"][1] == state["caret"][1]:
                    return False
                expanded = dict(actual)
                return expanded

            SESSION.until("async clipboard expanded accessory changed actual native caret Y", ready)
            x, y, width, height = expanded["caret"]
            rectangle = re.compile(r"\.set_cursor_rectangle\(" +
                                   rf"{x},\s*{y},\s*{width},\s*{height}\)")
            await_native_frame(window, offset, rectangle)
            evidence["native_states"][label] = unchanged_document(expanded)
            evidence["captures"].append(str(capture_window(window, dest, label + "-destination-bounded-preview")))
            # ModeLine has no InputArea. Clear focus there, then reverse native
            # traversal wraps to the composer's last controls: Cancel, Separate,
            # Insert. This avoids the editor's configured Tab completion binding.
            marker = GUI_DEBUG[window][1].stat().st_size
            click(window, bounds["WIDTH"] / 2, bounds["HEIGHT"] - 13 * args.zoom)
            await_native_frame(window, marker,
                               re.compile(r"wl_pointer[@#]\d+\.button\(\d+,\s*\d+,\s*272,\s*0\)"))
            SESSION.until("pending-paste native Input focus released", lambda: native_input_state(window) is None)
            framed_keys(window, *(["shift+Tab"] * (3 - index)))
            assert native_input_state(window) is None, "Clipboard confirmation did not retain native button focus"
            evidence["captures"].append(str(capture_window(window, dest, label + "-confirmation-button-focused")))
            framed_keys(window, "Return")

        confirm_paste(original_state, "paste-cancel", 2)
        after_cancel = unchanged_document(original_state)
        assert after_cancel["caret"] == original_state["caret"], (original_state, after_cancel)
        evidence["native_states"]["cancel_restored"] = after_cancel
        exact_wire("cancel-no-send")
        exact_rows()

        framed_keys(window, "ctrl+Home", "ctrl+shift+End")
        selected = _composer_native(window, original, length, 0)
        assert selected["caret"][1] == original_state["caret"][1], (original_state, selected)
        evidence["native_states"]["whole_original_native_selection"] = selected
        evidence["captures"].append(str(capture_window(window, dest, "paste-whole-original-native-selection")))
        confirm_paste(selected, "paste-insert", 0)
        inserted_length = len(normalized.encode("utf-8"))
        inserted = _composer_native(window, normalized, inserted_length, inserted_length)
        evidence["native_states"]["insert_exact_normalized_replacement"] = inserted
        evidence["captures"].append(str(capture_window(window, dest, "paste-insert-exact-native-replacement")))
        exact_wire("insert-no-send")
        exact_rows()
        framed_keys(window, "ctrl+z")  # ONE real native Undo, no compensating edits.
        undone = _composer_native(window, original, length, 0)
        assert undone["caret"] == selected["caret"], (selected, undone)
        evidence["native_states"]["one_native_undo_restored_selection"] = undone
        evidence["captures"].append(str(capture_window(window, dest, "paste-one-native-undo-restored-selection")))

        confirm_paste(undone, "paste-separate", 1)
        for text in separate:
            receive_own(text)
        retained = _composer_native(window, original, length, 0)
        assert retained["caret"] == selected["caret"], (selected, retained)
        evidence["native_states"]["separate_original_selection_retained"] = retained
        evidence["captures"].append(str(capture_window(window, dest, "paste-separate-original-draft-retained")))
        exact_wire("five-literal-separate")
        exact_rows()

        framed_keys(window, "Return")
        receive_own(original, seed_id if supported else "")
        _composer_native(window, "", 0, 0)
        exact_wire("original-draft-send")
        exact_rows()
        if supported:
            reply = begin_native_reply(window, layout, args.zoom, dest,
                                       label="reply-reselected", search=True)
            evidence["captures"].extend(reply["captures"])
            cancelled_text = "cancelled reply"
            type_text(cancelled_text)
            cancelled_length = len(cancelled_text.encode("utf-8"))
            _composer_native(window, cancelled_text, cancelled_length, cancelled_length)
            cancelled = cancel_native_reply(window, dest, label="reply-real-cancel",
                                            expected_text=cancelled_text)
            evidence["captures"].extend(cancelled["captures"])
            framed_keys(window, "Return")
            receive_own(cancelled_text)
            _composer_native(window, "", 0, 0)

        final = save_state(proc, prefs, db)
        assert final[0] == saved[0] and final[1] == (query_id,), (saved, final)
        assert [(row[0], row[3]) for row in final[2]] == [(row[0], row[3]) for row in saved[2]], (
            "Composer probe changed a pre-existing draft", saved[2], final[2])
        wait_rows(proc, db, "SELECT draft FROM buffers WHERE id=?", (query_id,), [("",)])
        exact_wire("final-no-unintended-send")
        exact_rows()
        metadata = database.execute(
            "SELECT buffer,nick,text,reply,pending FROM messages WHERE seq>? AND presence=0 "
            "ORDER BY buffer,nick,text,reply,pending", (frontier,)).fetchall()
        expected_metadata = sorted((buffer, nick, text,
                                    seed_id if supported and own and text == original else "", pending)
                                   for buffer, nick, text, own, pending in expected)
        assert metadata == expected_metadata, (expected_metadata, metadata)
        assert len(expected) == (8 if supported else 7), expected
        evidence.update({"exact_new_nonpresence_rows": expected,
                         "exact_own_payload_wire_sequence": expected_wire,
                         "exact_new_reply_pending_metadata": metadata,
                         "old_history_unchanged": len(old_history),
                         "final_native": _composer_native(window, "", 0, 0),
                         "limits": "Actual fixed-size native interaction proof; no OCR assertion of label glyphs. "
                                   "Reply context/cancel and bounded destination/preview are real captures; "
                                   "semantic behavior is proved by exact TLS tags/bytes and SQLite rows. "
                                   "Typing TAGMSG/read-marker metadata is not a payload send."})
        print("Native real-wire reply/bulk-paste proof: " +
              lab._redact(json.dumps(evidence, ensure_ascii=False)), flush=True)
        return expected
    finally:
        database.close()


def probe_keyboard_reply(proc, window, prefs, db, args, dest, observer, *,
                         query_id, query_target, own_nick, cleanup, lab, receive):
    """Prove real C-p/C-n/C-r replies and send-time raw-draft fallback; return exact rows."""
    _composer_native(window, "", 0, 0)
    saved = save_state(proc, prefs, db)
    assert saved[1] == (query_id,), saved
    preferences = tomllib.loads(prefs.read_text())
    assert client_tags_allowed(observer, "reply"), (
        "Keyboard reply needs real observer message-tags and permitted +reply")
    assert "echo-message" in observer.capabilities, "Real peer echo evidence requires echo-message"

    def barrier(label):
        token = "astra-keyboard-" + label + "-" + os.urandom(6).hex()
        observer.send("PING :" + token)
        observer.wait(lambda command, params, prefix:
                      command == "PONG" and token in params, timeout=15)

    barrier("start")
    wire_start = len(observer.frames)
    database = sqlite3.connect(f"file:{db}?mode=ro", uri=True)
    try:
        frontier = database.execute("SELECT COALESCE(MAX(seq),0) FROM messages").fetchone()[0]
        old_query = "SELECT * FROM messages WHERE seq<=? ORDER BY seq"
        old_history = database.execute(old_query, (frontier,)).fetchall()
        old_reactions = database.execute("SELECT * FROM reactions ORDER BY message,emoji,nick").fetchall()
        protected = database.execute("SELECT * FROM buffers WHERE id<>? ORDER BY id",
                                     (query_id,)).fetchall()
        new_query = ("SELECT buffer,nick,text,own,pending FROM messages "
                     "WHERE seq>? AND presence=0 ORDER BY buffer,nick,text,own,pending")
        nonce = os.urandom(5).hex()
        expected, own_frames, seeds = [], [], []
        reply_names = ("+reply", "+draft/reply", "reply", "draft/reply")
        evidence = {"native_states": {}, "captures": [], "capability_commands": [],
                    "echo_core_barriers": [], "observer_capabilities": sorted(observer.capabilities)}

        def native(label, text, cursor, anchor, *, expected_state=None):
            state = observe_native_input(window, dest, "keyboard-reply-" + label,
                                         text, cursor, anchor, expected=expected_state)
            evidence["native_states"][label] = state
            evidence["captures"].append(str(dest.with_name(
                dest.stem + "-keyboard-reply-" + label + ".png")))
            return state

        def exact_rows():
            wait_rows(proc, db, new_query, (frontier,), sorted(expected))
            assert database.execute(old_query, (frontier,)).fetchall() == old_history, (
                "Keyboard reply mutated or deleted old history")
            assert database.execute("SELECT * FROM reactions ORDER BY message,emoji,nick").fetchall() == old_reactions

        def own(text, reply=""):
            receive("PRIVMSG", [query_target, text], own_nick)
            frame = observer.frames[-1]
            assert (frame["command"], frame["params"], frame["prefix"].split("!", 1)[0]) == (
                "PRIVMSG", [query_target, text], own_nick), frame
            tags = {name: frame["tags"][name] for name in reply_names if name in frame["tags"]}
            assert tags == ({"+reply": reply} if reply else {}), frame
            own_frames.append(frame)
            expected.append((query_id, own_nick, text, 1, 0))
            # Peer delivery alone does not prove that the GUI's Core processed its echo.
            wait_rows(proc, db,
                      "SELECT pending,local_echo,reply,deleted FROM messages "
                      "WHERE seq>? AND buffer=? AND nick=? AND text=? AND own=1",
                      (frontier, query_id, own_nick, text), [(0, 0, reply, 0)])
            exact_rows()
            return frame

        def cap(command_text, label):
            _composer_native(window, "", 0, 0)
            type_text(command_text)
            native(label + "-public-cap-command", command_text,
                   len(command_text.encode("utf-8")), len(command_text.encode("utf-8")))
            framed_keys(window, "Return")
            _composer_native(window, "", 0, 0)
            text = f"Astra keyboard CAP {label} echo barrier {nonce}"
            type_text(text)
            framed_keys(window, "Return")
            frame = own(text)
            row = database.execute(
                "SELECT seq,id,buffer,nick,text,own,pending,local_echo,reply FROM messages "
                "WHERE seq>? AND buffer=? AND nick=? AND text=? AND own=1",
                (frontier, query_id, own_nick, text)).fetchall()
            evidence["capability_commands"].append(command_text)
            evidence["echo_core_barriers"].append({"command": command_text, "peer_wire": frame,
                                                   "persisted_server_echo": row})
            native(label + "-echo-core-barrier", "", 0, 0)

        def seed(label, *, referenceable=False):
            text = f"Astra keyboard reply peer {label} {nonce}"
            observer.send(f"PRIVMSG {own_nick} :{text}")
            receive("PRIVMSG", [own_nick, text], query_target)
            frame = observer.frames[-1]
            expected.append((query_id, query_target, text, 0, 0))
            exact_rows()
            rows = database.execute(
                "SELECT seq,id,time,reply,pending,local_echo,deleted,presence FROM messages "
                "WHERE seq>? AND buffer=? AND nick=? AND text=? AND own=0",
                (frontier, query_id, query_target, text)).fetchall()
            assert len(rows) == 1 and rows[0][3:] == ("", 0, 0, 0, 0), rows
            if referenceable:
                assert rows[0][1] and not rows[0][1].startswith("local:"), rows
                assert frame["tags"].get("msgid") == rows[0][1], (frame, rows)
            acknowledgment = wait_read_marker(proc, db, query_id)
            assert acknowledgment[3] == text, acknowledgment
            seeds.append({"label": label, "text": text, "peer_echo": frame,
                          "persisted": rows, "read_acknowledgment": acknowledgment})
            return rows[0][1]

        def select(label, chord):
            framed_keys(window, chord)
            SESSION.until("keyboard reply transcript selection " + label,
                          lambda: native_input_state(window) is None)
            evidence["captures"].append(str(capture_window(
                window, dest, "keyboard-reply-" + label)))

        fallback_id = seed("fallback-referenceable", referenceable=True)
        cap("/CAP REQ :-message-tags", "withdrawn")
        original = f"Keep café \x02raw bold\x02 /quit 猫🙂 {nonce}"
        seed_clipboard(original, cleanup)
        framed_keys(window, "ctrl+v")
        length = len(original.encode("utf-8"))
        native("fallback-original-raw", original, length, length)
        # Select the document's first four raw bytes, not a rich visual-line segment:
        # native Ctrl+Home sets byte zero directly; bare Home uses shaped line stops.
        framed_keys(window, "ctrl+Home", "shift+Right", "shift+Right", "shift+Right", "shift+Right")
        original_state = native("fallback-original-selection", original, 4, 0)
        select("fallback-cp-own-barrier", "ctrl+p")
        select("fallback-cp-referenceable-peer", "ctrl+p")
        assert database.execute("SELECT id FROM messages WHERE seq=?",
                                (seeds[0]["persisted"][0][0],)).fetchone() == (fallback_id,)
        framed_keys(window, "ctrl+r")
        native("fallback-quote-raw-selection", original, 4, 0, expected_state=original_state)
        # Read the actual seeded parent body above Input, not a reply-heading wording pin.
        from PIL import Image
        quote_capture = native_rgb_capture(window)
        quote_image = Image.frombytes("RGB", (quote_capture["width"], quote_capture["height"]),
                                      quote_capture["pixels"])
        x, y, width, height = native_reply_excerpt_region(
            window, prefs, dest, "keyboard-fallback-parent-quote")
        quote_path = dest.with_name(dest.stem + "-keyboard-fallback-parent-quote.png")
        quote_image.crop((round(x), round(y), round(x + width), round(y + height))).save(quote_path)
        quote_text = subprocess.run(["tesseract", str(quote_path), "stdout", "--psm", "6"],
                                    check=True, capture_output=True, text=True, timeout=15).stdout
        # The random nonce is wire evidence; its glyph recognition is not required.
        parent_words = set(re.findall(r"[a-z]+", seeds[0]["text"].rsplit(" ", 1)[0].casefold()))
        quote_words = set(re.findall(r"[a-z]+", quote_text.casefold()))
        assert parent_words <= quote_words, (seeds[0]["text"], quote_text)
        evidence["fallback_parent_quote"] = {"source_body": seeds[0]["text"],
                                             "recognized": quote_text, "capture": str(quote_path)}
        evidence["captures"].append(str(quote_path))
        cancelled = cancel_native_reply(window, dest, label="keyboard-fallback-cancel",
                                        expected_text=original)
        evidence["fallback_cancel"] = cancelled
        evidence["captures"].extend(cancelled["captures"])
        native("fallback-cancel-preserved-selection", original, 4, 0,
               expected_state=original_state)
        # Cancel returns focus to Input, so C-p starts again at the newest row.
        select("fallback-reselect-own-barrier", "ctrl+p")
        select("fallback-reselect-referenceable-peer", "ctrl+p")
        framed_keys(window, "ctrl+r")
        native("fallback-reselected-quote-raw", original, 4, 0, expected_state=original_state)
        literal = query_target + ": " + original
        exact_rows()
        barrier("fallback-before-send")
        assert [frame for frame in observer.frames[wire_start:] if
                frame["prefix"].split("!", 1)[0] == own_nick and frame["command"] == "PRIVMSG"] == own_frames
        framed_keys(window, "Return")
        fallback_frame = own(literal)
        native("fallback-literal-sent", "", 0, 0)
        # Sending clears the editor once; ONE native Undo restores raw bytes and selection.
        framed_keys(window, "ctrl+z")
        native("fallback-one-undo-after-send", original, 4, 0, expected_state=original_state)
        # C-a is the configured beginning-of-line action, not native Select All.
        framed_keys(window, "ctrl+Home", "ctrl+shift+End")
        native("fallback-restored-draft-selected", original, len(original.encode("utf-8")), 0)
        framed_keys(window, "BackSpace")
        native("fallback-restored-draft-cleared", "", 0, 0)

        cap("/CAP REQ :message-tags", "restored")
        older_id = seed("supported-older", referenceable=True)
        newer_id = seed("supported-newer", referenceable=True)
        reply_by_text, supported_frames = {}, []
        # Distinct real sends make a no-op C-p or C-n fail the exact +reply wire.
        # After the older reply is sent, it is the newest row, ahead of both peers.
        for label, selected_id, navigation in (
                ("older", older_id, ("ctrl+p", "ctrl+p")),
                ("newer", newer_id, ("ctrl+p", "ctrl+p", "ctrl+p", "ctrl+n"))):
            supported = f"Keep my café keyboard {label} reply {nonce}"
            type_text(supported)
            length = len(supported.encode("utf-8"))
            native(label + "-original", supported, length, length)
            framed_keys(window, "ctrl+Home", "shift+Right", "shift+Right", "shift+Right", "shift+Right")
            native(label + "-original-selection", supported, 4, 0)
            for step, chord in enumerate(navigation):
                select(f"supported-{label}-{step}-{chord}", chord)
            framed_keys(window, "ctrl+r")
            native(label + "-context-preserved-selection", supported, 4, 0)
            exact_rows()
            framed_keys(window, "Return")
            supported_frame = own(supported, selected_id)
            supported_frames.append(supported_frame)
            reply_by_text[supported] = selected_id
            native(label + "-tagged-sent", "", 0, 0)
            # A restore NAK removes the local capability too; only positive
            # peer-observed +reply proves restoration, never the typed CAP command.
            assert supported_frame["tags"]["+reply"] == selected_id, supported_frame

        final = save_state(proc, prefs, db)
        assert final[:2] == saved[:2], (saved, final)
        assert [(row[0], row[3]) for row in final[2]] == [
            (row[0], row[3]) for row in saved[2]], (saved, final)
        assert database.execute("SELECT * FROM buffers WHERE id<>? ORDER BY id",
                                (query_id,)).fetchall() == protected, (
            "Keyboard reply changed another buffer's draft/read marker/unread")
        assert tomllib.loads(prefs.read_text()) == preferences, "Keyboard reply changed saved preferences"
        wait_rows(proc, db, "SELECT draft FROM buffers WHERE id=?", (query_id,), [("",)])
        exact_rows()
        metadata = database.execute(
            "SELECT buffer,nick,text,own,pending,reply,local_echo,deleted,presence "
            "FROM messages WHERE seq>? AND presence=0 ORDER BY buffer,nick,text,own,pending",
            (frontier,)).fetchall()
        assert metadata == sorted((*row, reply_by_text.get(row[2], ""), 0, 0, 0)
                                  for row in expected), metadata
        barrier("finish")
        payload_frames = [frame for frame in observer.frames[wire_start:]
                          if frame["prefix"].split("!", 1)[0] == own_nick and
                          (frame["command"] in ("PRIVMSG", "NOTICE", "QUIT", "PART", "JOIN", "NICK")
                           or any(name in frame["tags"] for name in reply_names))]
        assert payload_frames == own_frames, lab._redact(repr(payload_frames))
        assert len(expected) == 8, expected
        evidence.update({"seeds": seeds, "original_raw_draft": original,
                         "fallback_literal_wire": fallback_frame, "positive_restored_replies": supported_frames,
                         "exact_own_payload_wire": payload_frames, "exact_new_nonpresence_rows": expected,
                         "exact_new_metadata": metadata, "old_history_unchanged": len(old_history),
                         "workspace_before": saved, "workspace_after": final,
                         "final_native": _composer_native(window, "", 0, 0),
                         "limits": "CAP commands run on the actual GUI connection. Its CAP ACK frames "
                                   "are not directly captured: ordered own server echoes persisted with "
                                   "pending=0/local_echo=0 are Core barriers; fallback and positive restored "
                                   "+reply wire independently prove policy. No Model hooks or synthetic ACKs."})
        evidence_path = dest.with_name(dest.stem + "-keyboard-reply-evidence.json")
        evidence_path.write_text(json.dumps(evidence, indent=2, ensure_ascii=False) + "\n")
        print("Native real-wire keyboard reply proof:", evidence_path, flush=True)
        return expected
    finally:
        database.close()


def probe_reaction_picker(proc, window, prefs, db, args, dest, observer, *,
                          query_id, query_target, own_nick, lab, receive):
    """Drive the real message picker; return only its two ordinary message rows."""
    from PIL import Image

    _composer_native(window, "", 0, 0)
    saved = save_state(proc, prefs, db)
    assert saved[1] == (query_id,), saved
    assert client_tags_allowed(observer, "reply", "draft/react"), (
        "Reaction picker requires actual ACKed message-tags and permitted client tags",
        sorted(observer.capabilities), observer.isupport.get("CLIENTTAGDENY"))

    def barrier(label):
        token = "astra-reaction-" + label + "-" + os.urandom(6).hex()
        observer.send("PING :" + token)
        observer.wait(lambda command, params, prefix:
                      command == "PONG" and token in params, timeout=15)

    barrier("start")
    wire_start = len(observer.frames)
    database = sqlite3.connect(f"file:{db}?mode=ro", uri=True)
    try:
        frontier = database.execute("SELECT COALESCE(MAX(seq),0) FROM messages").fetchone()[0]
        old_query = "SELECT * FROM messages WHERE seq<=? ORDER BY seq"
        old_history = database.execute(old_query, (frontier,)).fetchall()
        reaction_query = "SELECT message,emoji,nick,ordinal FROM reactions ORDER BY message,emoji,nick"
        old_reactions = database.execute(reaction_query).fetchall()
        new_query = ("SELECT buffer,nick,text,own,pending FROM messages "
                     "WHERE seq>? AND presence=0 ORDER BY buffer,nick,text,own,pending")
        # Focused native text controls own C-e. Exercise a UTF-8 multiline
        # first-line end before the unchanged transcript-focus reaction proof.
        first_line = "first café e\u0301 猫🙂"
        multiline = first_line + "\nsecond line"
        type_text(first_line)
        framed_keys(window, "shift+Return")
        type_text("second line")
        total_bytes, line_bytes = len(multiline.encode("utf-8")), len(first_line.encode("utf-8"))
        framed_keys(window, "Left", "Right")
        second_line_end = _composer_native(window, multiline, total_bytes, total_bytes)
        framed_keys(window, "ctrl+Home", "Right")
        inside_line = _composer_native(window, multiline, 1, 1)
        framed_keys(window, "ctrl+e")
        line_end = _composer_native(window, multiline, line_bytes, line_bytes)
        first_row = (inside_line["caret"][1], inside_line["caret"][1] + inside_line["caret"][3])
        end_row = (line_end["caret"][1], line_end["caret"][1] + line_end["caret"][3])
        next_row = (second_line_end["caret"][1], second_line_end["caret"][1] + second_line_end["caret"][3])
        # Native caret ascenders/heights vary with the shaped neighboring
        # glyph. Row overlap, not identical top pixels, establishes same line.
        assert max(first_row[0], end_row[0]) < min(first_row[1], end_row[1]), (
            "C-e left the current native text row", inside_line, line_end)
        assert end_row[1] <= next_row[0] or next_row[1] <= end_row[0], (
            "C-e reached the measured second text row", line_end, second_line_end)
        assert line_end["caret"][0] > inside_line["caret"][0], (inside_line, line_end)
        # C-a is Emacs line-beginning in the actual profile, not Select All.
        # Select the complete document through explicit native endpoints.
        framed_keys(window, "ctrl+Home", "ctrl+shift+End")
        copied_selection = _composer_native(window, multiline, total_bytes, 0)
        framed_keys(window, "ctrl+c")
        SESSION.until("native C-e retains exact multiline UTF-8 clipboard bytes",
                      lambda: clipboard_text() == multiline)
        copied_multiline = clipboard_text()
        eol_capture = capture_window(window, dest, "reaction-textbox-ctrl-e-line-end")
        barrier("textbox-ctrl-e")
        action_names = ("+draft/react", "draft/react", "+draft/unreact", "draft/unreact")
        unexpected_textbox_actions = [frame for frame in observer.frames[wire_start:]
            if frame["prefix"].split("!", 1)[0] == own_nick and
            (frame["command"] == "PRIVMSG" or
             frame["command"] == "TAGMSG" and any(name in frame["tags"] for name in action_names))]
        assert not unexpected_textbox_actions, lab._redact(repr(unexpected_textbox_actions))
        assert database.execute(new_query, (frontier,)).fetchall() == [], "Text C-e created message history"
        assert database.execute(old_query, (frontier,)).fetchall() == old_history, "Text C-e mutated retained history"
        assert database.execute(reaction_query).fetchall() == old_reactions, "Text C-e changed reactions"
        text_eol_proof = {"raw": multiline, "first_line_utf8_bytes": line_bytes,
                         "runtime_bindings": tomllib.loads(Path(prefs).read_text())["bindings"],
                         "inside_first_line": inside_line, "current_line_end": line_end,
                         "second_line_end": second_line_end,
                         "copied_selection": copied_selection,
                         "copied_raw": copied_multiline, "no_message_or_reaction_wire": True,
                         "capture": str(eol_capture)}
        clear_draft()
        _composer_native(window, "", 0, 0)
        nonce = os.urandom(5).hex()
        seed_text = f"Astra real reaction picker {nonce}: newest referenceable peer message."
        expected = [(query_id, query_target, seed_text, 0, 0)]
        observer.send(f"PRIVMSG {own_nick} :{seed_text}")
        seed_echo = None
        if "echo-message" in observer.capabilities:
            receive("PRIVMSG", [own_nick, seed_text], query_target)
            seed_echo = dict(observer.last_tags)
        wait_rows(proc, db, new_query, (frontier,), sorted(expected))
        seed = database.execute(
            "SELECT seq,id,time,reply,pending,deleted,presence FROM messages "
            "WHERE seq>? AND buffer=? AND nick=? AND text=? AND own=0",
            (frontier, query_id, query_target, seed_text)).fetchall()
        assert len(seed) == 1, seed
        sequence, seed_id, seed_time, reply, pending, deleted, presence = seed[0]
        assert seed_id and not seed_id.startswith("local:"), (
            "Reaction picker needs a real server-assigned referenceable seed ID", seed)
        assert (reply, pending, deleted, presence) == ("", 0, 0, 0), seed
        if seed_echo is not None:
            if "msgid" in seed_echo:
                assert seed_echo["msgid"] == seed_id, (seed, seed_echo)
            if "time" in seed_echo:
                assert seed_echo["time"] == seed_time, (seed, seed_echo)
        acknowledged = wait_read_marker(proc, db, query_id)
        assert acknowledged[2:4] == (seed_id, seed_text), acknowledged
        assert database.execute(
            "SELECT seq,id FROM messages WHERE buffer=? AND presence=0 ORDER BY seq DESC LIMIT 1",
            (query_id,)).fetchone() == (sequence, seed_id), seed

        original = "Keep my café reaction draft " + nonce
        type_text(original)
        length = len(original.encode("utf-8"))
        _composer_native(window, original, length, length)
        framed_keys(window, "Home", "shift+Right", "shift+Right", "shift+Right", "shift+Right")
        original_state = observe_native_input(
            window, dest, "reaction-picker-original-selection", original, 4, 0)
        framed_keys(window, "ctrl+p")
        SESSION.until("newest real message selected with non-text transcript focus",
                      lambda: native_input_state(window) is None)
        framed_keys(window, "ctrl+e")
        opened = observe_native_input(window, dest, "reaction-picker-open", "", 0, 0)
        assert opened["caret"] != original_state["caret"], (
            "Reaction picker did not move the actual native text client to search",
            original_state, opened)
        # The exact thumbsup query has one match. Prove real result movement on
        # the initial multi-result catalog, then filter to the reaction to send.
        framed_keys(window, "ctrl+n")
        next_result = observe_native_input(
            window, dest, "reaction-picker-next-result", "", 0, 0, expected=opened)
        framed_keys(window, "ctrl+p")
        previous_result = observe_native_input(
            window, dest, "reaction-picker-previous-result", "", 0, 0, expected=opened)
        bounds = window_geometry(window)
        width, height, scale = bounds["WIDTH"], bounds["HEIGHT"], args.zoom
        margin = min(8 * scale, width / 8, height / 8)
        panel_width, panel_height = min(384 * scale, width - 2 * margin), min(
            450 * scale, height - 2 * margin)
        panel_x, panel_y = (width - panel_width) / 2, (height - panel_height) / 2
        # Picker::geometry centers the panel; CompletionView's first two rows
        # start at panel (16,84)*scale. No caret/clock/animated region is included.
        result_rect = tuple(round(value) for value in (
            panel_x + 16 * scale, panel_y + 84 * scale,
            panel_x + panel_width - 16 * scale,
            min(panel_y + 172 * scale, panel_y + panel_height - 16 * scale)))
        result_pixels = []
        for label in ("open", "next-result", "previous-result"):
            with Image.open(dest.with_name(dest.stem + "-reaction-picker-" + label + ".png")) as image:
                result_pixels.append(image.convert("RGB").crop(result_rect).tobytes())
        assert result_pixels[0] != result_pixels[1], (
            "C-n did not visibly move the real picker result highlight", result_rect)
        assert result_pixels[2] == result_pixels[0], (
            "C-p did not restore the real picker result highlight", result_rect)
        type_text("thumbsup")
        filtered = observe_native_input(window, dest, "reaction-picker-filter", "thumbsup", 8, 8)
        wait_rows(proc, db, new_query, (frontier,), sorted(expected))
        assert database.execute(reaction_query).fetchall() == old_reactions, (
            "Search/navigation sent a reaction before acceptance")
        framed_keys(window, "Return")
        reaction_action_names = ("+draft/react", "draft/react", "+draft/unreact", "draft/unreact")
        # Typing notifications share TAGMSG and can already be queued from the
        # preserved draft. Match the action kind, then assert every wire value.
        observer.wait(lambda command, params, prefix:
                      command == "TAGMSG" and any(name in observer.last_tags
                                                  for name in reaction_action_names), timeout=15)
        reaction_frame = observer.frames[-1]
        relevant_names = ("+reply", "+draft/reply", "reply", "draft/reply",
                          "+draft/react", "draft/react", "+draft/unreact", "draft/unreact")
        reaction_tags = {name: reaction_frame["tags"][name] for name in relevant_names
                         if name in reaction_frame["tags"]}
        assert (reaction_frame["command"], reaction_frame["params"],
                reaction_frame["prefix"].split("!", 1)[0], reaction_tags) == (
                    "TAGMSG", [query_target], own_nick,
                    {"+reply": seed_id, "+draft/react": "👍"}), reaction_frame
        wait_rows(proc, db,
                  "SELECT emoji,nick FROM reactions WHERE message=? ORDER BY emoji,nick",
                  (sequence,), [("👍", own_nick)])
        reaction_rows = database.execute(reaction_query).fetchall()
        assert [row for row in reaction_rows if row[0] != sequence] == old_reactions, (
            "Picker changed another message's reactions", old_reactions, reaction_rows)
        SESSION.until("accepted reaction restores original non-text transcript focus",
                      lambda: native_input_state(window) is None)
        capture_window(window, dest, "reaction-picker-accepted")
        # The picker restores its transcript origin, not an unrelated Composer
        # focus. Leave that observed mode through its real keyboard route.
        framed_keys(window, "ctrl+g")
        restored = observe_native_input(
            window, dest, "reaction-picker-composer-restored", original, 4, 0, expected=original_state)
        wait_rows(proc, db, new_query, (frontier,), sorted(expected))
        framed_keys(window, "Return")
        receive("PRIVMSG", [query_target, original], own_nick)
        draft_frame = observer.frames[-1]
        assert not any(name in draft_frame["tags"] for name in relevant_names), draft_frame
        expected.append((query_id, own_nick, original, 1, 0))
        empty = observe_native_input(window, dest, "reaction-picker-original-draft-sent", "", 0, 0)
        final = save_state(proc, prefs, db)
        assert final[0] == saved[0] and final[1] == saved[1], (saved, final)
        assert [row for row in final[2] if row[0] != query_id] == [
            row for row in saved[2] if row[0] != query_id], (saved, final)
        assert [(row[0], row[3]) for row in final[2]] == [
            (row[0], row[3]) for row in saved[2]], (saved, final)
        wait_rows(proc, db, "SELECT draft FROM buffers WHERE id=?", (query_id,), [("",)])
        wait_rows(proc, db, new_query, (frontier,), sorted(expected))
        assert database.execute(old_query, (frontier,)).fetchall() == old_history, (
            "Reaction picker changed or deleted existing message history")
        metadata = database.execute(
            "SELECT buffer,nick,text,own,pending,reply,deleted FROM messages WHERE seq>? "
            "ORDER BY buffer,nick,text,own,pending", (frontier,)).fetchall()
        assert metadata == sorted((*row, "", 0) for row in expected), metadata
        assert database.execute(reaction_query).fetchall() == reaction_rows, reaction_rows
        barrier("finish")
        own_frames = [frame for frame in observer.frames[wire_start:]
                      if frame["prefix"].split("!", 1)[0] == own_nick and
                      (frame["command"] in ("PRIVMSG", "NOTICE", "QUIT", "PART", "JOIN", "NICK")
                       or any(name in frame["tags"] for name in relevant_names))]
        assert own_frames == [reaction_frame, draft_frame], lab._redact(repr(own_frames))
        reaction_frames = [frame for frame in observer.frames[wire_start:]
                           if frame["command"] == "TAGMSG" and
                           any(name in frame["tags"] for name in reaction_action_names)]
        assert reaction_frames == [reaction_frame], lab._redact(repr(reaction_frames))
        evidence = {
            "capabilities_ack": sorted(observer.capabilities),
            "clienttagdeny": observer.isupport.get("CLIENTTAGDENY"),
            "focused_textbox_ctrl_e": text_eol_proof,
            "seed": {"seq": sequence, "id": seed_id, "time": seed_time, "text": seed_text,
                     "observer_echo_tags": seed_echo, "read_acknowledgment": acknowledged},
            "native_states": {"original_selection": original_state, "search_open": opened,
                              "filtered": filtered, "next_result": next_result,
                              "previous_result": previous_result, "accepted": None,
                              "restored": restored, "empty": empty},
            "result_navigation": {"rect": result_rect,
                                  "sha256": [hashlib.sha256(pixels).hexdigest()
                                             for pixels in result_pixels],
                                  "query": "", "next_changes_pixels": True,
                                  "previous_restores_pixels": True},
            "exact_own_wire_frames": own_frames,
            "exact_reaction_wire_frames": reaction_frames,
            "nonreaction_tagmsg_frames": [
                frame for frame in observer.frames[wire_start:] if frame["command"] == "TAGMSG"
                and not any(name in frame["tags"] for name in reaction_action_names)],
            "persisted_reactions": reaction_rows,
            "exact_new_nonpresence_rows": expected,
            "exact_new_message_metadata": metadata,
            "old_history_unchanged": len(old_history),
            "workspace_before": saved, "workspace_after": final,
            "captures": [str(dest.with_name(dest.stem + "-reaction-picker-" + label + ".png"))
                         for label in ("original-selection", "open", "filter", "next-result",
                                       "previous-result", "accepted", "composer-restored",
                                       "original-draft-sent")],
            "limits": "Real native search/result keys and PNGs; exact selected source is proved by "
                      "peer-observed TAGMSG and separate SQLite reaction, not OCR or callbacks.",
        }
        evidence_path = dest.with_name(dest.stem + "-reaction-picker-evidence.json")
        evidence_path.write_text(json.dumps(evidence, indent=2, ensure_ascii=False) + "\n")
        print("Native real-wire reaction picker proof:", evidence_path, flush=True)
        return expected
    finally:
        database.close()




def probe_cursor_controls_dpi(proc, window, prefs, db, args, dest):
    assert args.size == (1440, 940) and args.zoom == 1, args
    prefs, db, dest = Path(prefs), Path(db), Path(dest)
    assert prefs.resolve().is_relative_to(Path(SESSION.env['XDG_CONFIG_HOME']).resolve()), prefs
    assert GUI_DEBUG[window][0] is proc and proc.poll() is None
    debug = GUI_DEBUG[window][1]
    ids = main_surface_ids(debug)
    assert ids[0] == GUI_DEBUG[window][2], (ids, GUI_DEBUG[window])
    original_env, original_session_env = dict(os.environ), dict(SESSION.env)
    process_env = Path(f'/proc/{proc.pid}/environ').read_bytes()
    baseline = tomllib.loads(prefs.read_text())
    bindings = baseline['bindings'].copy()
    for name, value in (('execute-command', 'M-x'), ('backward-char', 'C-b'),
                        ('forward-char', 'C-f'), ('set-mark', 'C-SPC'),
                        ('end-of-buffer', 'M->'), ('settings', 'C-,')):
        assert bindings[name] == value, (name, bindings[name], value)
    assert baseline['appearance']['scale'] == 1
    before_cursor = baseline['cursor'].copy()
    appearance = baseline['appearance'].copy()
    audio = baseline['audio'].copy()
    initial = native_input_state(window)
    assert initial and initial['caret'] and initial['text'], initial
    text = initial['text']
    encoded = text.encode('utf-8')
    boundaries = [0]
    for character in text:
        boundaries.append(boundaries[-1] + len(character.encode('utf-8')))
    assert initial['cursor'] in boundaries and initial['anchor'] in boundaries, initial
    previous = boundaries[-2]
    keyboard_press = re.compile(r'wl_keyboard[@#]\d+\.key\(\d+,\s*\d+,\s*\d+,\s*1\)')
    original_node = native_window(window)
    identity = (original_node['id'], original_node['pid'], original_node['app_id'])
    assert identity == (window, proc.pid, 'io.github.trevarj.blastirc'), identity
    assert original_node['shell'] == 'xdg_shell' and original_node['border'] == 'none', original_node
    original_geometry = window_geometry(window)
    assert (original_geometry['WIDTH'], original_geometry['HEIGHT']) == (1440, 940), original_geometry
    original_fullscreen = original_node['fullscreen_mode']
    original_floating = original_node['floating'] in ('auto_on', 'user_on')
    criterion = f'[con_id={window}]'
    outputs = SESSION.ipc('get_outputs')
    assert {output['name'] for output in outputs if output['active']} == {'HEADLESS-1'}, outputs
    original_output = next(output for output in outputs if output['name'] == 'HEADLESS-1')
    assert original_output['scale'] == 1 and original_output['transform'] == 'normal', original_output
    assert (original_output['rect']['width'], original_output['rect']['height']) == (1440, 940)
    assert (original_output['current_mode']['width'], original_output['current_mode']['height']) == (1440, 940)
    report = {'beforeCursorPrefs': before_cursor, 'selectedCursorPrefs': None,
              'window_identity': identity, 'protocol_ids': ids,
              'native_before': initial, 'cursor_controls': [], 'suppression': [], 'dpi': [],
              'timed_movements': [],
              'protocol_log': str(debug), 'status': 'running',
              'limits': 'Timed native RGB captures distinguish Trail from Reduced/Effects Off. Comet/Ribbon/Pulse have real selector, persistence, editor-state and still-capture coverage only, not animation/distinctness proof. No private geometry is inferred from IME rectangles; physical IM engines and candidate windows are not exercised.'}

    def artifact(label, suffix):
        return dest.with_name(dest.stem + '-' + label + suffix)

    def alive():
        assert proc.poll() is None, f'Native client exited during cursor/DPI probe: {proc.returncode}'
        assert dict(os.environ) == original_env and dict(SESSION.env) == original_session_env
        assert Path(f'/proc/{proc.pid}/environ').read_bytes() == process_env
        actual = native_window(window)
        assert (actual['id'], actual['pid'], actual['app_id']) == identity, actual
        assert actual['shell'] == 'xdg_shell', actual

    def state(cursor, anchor):
        alive()
        actual = native_input_state(window)
        assert actual and (actual['text'], actual['cursor'], actual['anchor']) == (text, cursor, anchor), actual
        assert actual['caret'] is not None, actual
        x, y, width, height = actual['caret']
        geometry = window_geometry(window)
        assert width > 0 and height > 0 and x >= 0 and y >= 0, actual
        assert x + width <= geometry['WIDTH'] and y + height <= geometry['HEIGHT'], (actual, geometry)
        return actual

    def disk(label, cursor=None):
        saved_text = prefs.read_text()
        saved = tomllib.loads(saved_text)
        assert saved['bindings'] == bindings, 'Cursor probe changed authored bindings'
        assert saved['audio'] == audio, 'Cursor probe changed audio preferences'
        if cursor is not None:
            assert saved['cursor'] == cursor, (saved['cursor'], cursor)
        path = artifact(label + '-settings', '.toml')
        path.write_text(saved_text)
        return saved, str(path)

    def photo(label):
        alive()
        geometry = window_geometry(window)
        output = next(item for item in SESSION.ipc('get_outputs') if item['name'] == 'HEADLESS-1')
        scale = output['scale']
        path = artifact(label, '.png')
        region = f"{geometry['X']},{geometry['Y']} {geometry['WIDTH']}x{geometry['HEIGHT']}"
        pixels = tuple(round(geometry[name] * scale) for name in ('WIDTH', 'HEIGHT'))
        started = time.monotonic_ns()
        actual = capture_png(SESSION.env, path, scale=scale, expected_pixels=pixels, region=region)
        finished = time.monotonic_ns()
        assert window_geometry(window) == geometry, 'Native client moved during screenshot'
        return {'capture': str(path), 'physical_pixels': actual, 'scale': scale,
                'capture_started_ns': started, 'capture_finished_ns': finished}

    def collapse_at_end():
        # Native Home/Shift+End establishes a KNOWN active mark without an
        # unsupported M-x select-all entry. Do not cancel: that clears replies.
        framed_keys(window, 'ctrl+Home', 'ctrl+shift+End')
        state(len(encoded), 0)
        command('set-mark', window=window)
        command('end-of-buffer', window=window)
        return state(len(encoded), len(encoded))

    def restore_selection():
        collapse_at_end()
        anchor_index = boundaries.index(initial['anchor'])
        steps = len(boundaries) - 1 - anchor_index
        if steps:
            framed_keys(window, *(['ctrl+b'] * steps))
        state(initial['anchor'], initial['anchor'])
        if initial['cursor'] != initial['anchor']:
            command('set-mark', window=window)
            delta = boundaries.index(initial['cursor']) - anchor_index
            framed_keys(window, *(['ctrl+f' if delta > 0 else 'ctrl+b'] * abs(delta)))
        return state(initial['cursor'], initial['anchor'])

    def policy(fx, reduced, label):
        current = tomllib.loads(prefs.read_text())
        open_settings(window, prefs)
        framed_keys(window, 'shift+Tab')
        settings_category(window, args.zoom, 3)
        framed_keys(window, 'Tab')
        settings_select(window, ('off', 'subtle', 'full').index(fx))
        framed_keys(window, 'Tab')
        if current['appearance']['reduced_motion'] != reduced:
            framed_keys(window, 'Return')
        framed_keys(window, 'shift+Tab', 'shift+Tab')
        saved = save_settings_footer(window, saved_path=prefs, destination=dest, label=label + '-effective-settings')
        expected = {**appearance, 'fx': fx, 'reduced_motion': reduced}
        assert saved['appearance'] == expected, (saved['appearance'], expected)
        expected_cursor = report['selectedCursorPrefs'] or before_cursor
        assert saved['cursor'] == expected_cursor, (saved['cursor'], expected_cursor)
        saved, settings_path = disk(label, saved['cursor'])
        framed_keys(window, 'shift+Tab', 'Return')  # Real sticky Close from navigation.
        SETTINGS_OPEN[window] = False
        save_state(proc, prefs, db)
        wait_layout(proc, prefs, predicate=lambda layout: 'settings' not in layout)
        return {'fx': fx, 'reduced_motion': reduced, 'saved_settings': settings_path,
                'cursor_preferences': saved['cursor'], 'capture': photo(label + '-editor')}

    def trace(offset):
        data = debug.read_bytes()[offset:]
        patterns = (keyboard_press, re.compile(r'zwp_text_input_v3[@#]\d+\.(?:set_surrounding_text|set_cursor_rectangle|commit)\('),
                    re.compile(rf'wl_surface[@#]{ids[0]}\.(?:attach|commit|frame)\('),
                    re.compile(r'wl_callback[@#]\d+\.done\('))
        return [line for line in data.decode('utf-8', errors='replace').splitlines()
                if any(pattern.search(line) for pattern in patterns)]

    def move(chord, cursor, label, *, sample, expect_trail=False):
        before = native_input_state(window)
        assert before and before['caret'], before
        geometry = window_geometry(window)
        offset = debug.stat().st_size
        started = time.monotonic_ns()
        key(chord)
        returned = time.monotonic_ns()
        root_wait_started = time.monotonic_ns()
        await_native_frame(window, offset, keyboard_press)
        root_paint_observed = time.monotonic_ns()
        immediate = native_input_state(window, offset=offset, initial=before)
        native_state_observed = time.monotonic_ns()
        assert immediate and (immediate['text'], immediate['cursor'], immediate['anchor']) == (text, cursor, cursor), immediate
        assert immediate['caret'] is not None, immediate
        result = {'key': chord, 'key_call_started_ns': started, 'key_call_returned_ns': returned,
                  'native_before': before, 'native_immediately_after_input_frame': immediate,
                  'log_offset': offset, 'post_input_root_wait_started_ns': root_wait_started,
                  'post_input_root_paint_observed_ns': root_paint_observed,
                  'native_state_observed_ns': native_state_observed, 'samples': []}
        if sample:
            report['timed_movements'].append(result)
            duration_ns = round(report['selectedCursorPrefs']['trail_duration_ms'] * 1_000_000)
            result.update(configured_duration_ms=report['selectedCursorPrefs']['trail_duration_ms'],
                          expected_visible_trail=expect_trail, sampling_deadline_ms=300,
                          late_frame_bound_ns=root_paint_observed + duration_ns,
                          capture_clock='CLOCK_MONOTONIC: verified private wlroots 0.20.2 or EXT protocol')
            first, last = before['caret'], immediate['caret']
            assert first[:2] != last[:2], (
                'The real previous-codepoint step did not visibly displace the insertion point; '
                'this input cannot establish a cursor trail', first, last)
            margin = 2 * max(first[3], last[3])
            left = max(0, min(first[0], last[0]) - margin)
            top = max(0, min(first[1], last[1]) - margin)
            right = min(geometry['WIDTH'], max(first[0] + first[2], last[0] + last[2]) + margin)
            bottom = min(geometry['HEIGHT'], max(first[1] + first[3], last[1] + last[3]) + margin)
            assert left < right and top < bottom, (first, last, geometry)
            # Use both actual insertion positions and a bounded native-height
            # margin, not one-codepoint/monospace width assumptions. The exact
            # crop includes the shaped cell/path context but is not a claim
            # about private painted rectangles or native viewport geometry.
            result.update(crop_bounds=(left, top, right - left, bottom - top),
                          crop_expansion_logical_px=margin,
                          crop_source='actual before/after narrow caret rectangles at native output scale 1')
            pending = []
            settled = 0

            def post_deadline_root_render():
                # A new output timestamp can still show a pre-deadline client
                # buffer. Require a real later client tick and rendered root
                # attach/commit before requesting a settled screencopy.
                with debug.open('rb') as log:
                    log.seek(offset)
                    records = log.read().decode(errors='replace')
                root = GUI_DEBUG[window][2]
                callbacks = set(re.findall(
                    rf'wl_surface[@#]{root}\.frame\(new id wl_callback[@#](\d+)\)', records))
                deadline_ms = math.ceil((root_paint_observed + duration_ns) / 1_000_000)
                tick = rendered = None
                attached = False
                pattern = (rf'wl_callback[@#](\d+)\.done\((\d+)\)'
                           rf'|wl_surface[@#]{root}\.attach\(wl_buffer[@#]\d+,'
                           rf'|wl_surface[@#]{root}\.commit\(')
                for event in re.finditer(pattern, records):
                    if event[1] is not None:
                        # wl_callback.done is uint32 milliseconds; compare it
                        # to the same monotonic clock across its normal wrap.
                        elapsed = ((int(event[2]) - deadline_ms + (1 << 31)) % (1 << 32)) - (1 << 31)
                        if event[1] in callbacks and elapsed >= 0:
                            tick = int(event[2])
                            attached = False
                    elif '.attach(' in event[0]:
                        attached = tick is not None
                    elif attached:
                        rendered = {'callback_milliseconds32': tick,
                                    'root_surface': root, 'root_buffer_commit': event[0]}
                        attached = False
                if rendered is not None:
                    rendered['observed_monotonic_ns'] = time.monotonic_ns()
                return rendered

            # grim waits on actual screencopy frames: no settling sleep or
            # synthesized intermediate image. Stay strictly below blink time.
            while settled < 2:
                assert time.monotonic_ns() - started < 300_000_000, (
                    'Cursor sampling missed its <300ms blink-free observation window', result)
                late_render = post_deadline_root_render()
                shot_started = time.monotonic_ns()
                image = native_rgb_capture(window, geometry=geometry, capture_timing=True)
                shot_finished = time.monotonic_ns()
                timestamps = image['capture_timestamps']
                assert len(timestamps) == 1, (
                    'Timed cursor sampler requires one authoritative private output/capture timestamp', timestamps)
                frame_time = timestamps[0]['timestamp_ns']
                # EXT specifies system monotonic time. In our private wlroots 0.20.2,
                # output.c uses CLOCK_MONOTONIC for event.when; screencopy sends that
                # value in ready. Older/cached frames cannot certify this input.
                # https://gitlab.freedesktop.org/wlroots/wlroots/-/blob/0.20.2/types/output/output.c
                timestamp_usable = shot_started <= frame_time <= shot_finished
                result['last_capture'] = {'capture_started_ns': shot_started,
                                          'capture_finished_ns': shot_finished,
                                          'capture_timestamps': timestamps,
                                          'timestamp_usable': timestamp_usable}
                assert timestamp_usable, ('Native capture timestamp is stale or not on the process monotonic clock', result)
                assert frame_time - started < 300_000_000, (
                    'Native captured frame exceeded the <300ms blink-free observation window', result)
                assert image['geometry'] == geometry, 'Native client moved during timed cursor sampling'
                # Process completion includes readback/encoding/IO after the
                # image was captured. Bound the image by its real presentation
                # timestamp; missing/stale metadata cannot certify this window.
                in_window = frame_time - started < duration_ns
                after_duration = (late_render is not None and
                                  frame_time - root_paint_observed >= duration_ns)
                settled += int(after_duration)
                observed = native_input_state(window, offset=offset, initial=before)
                assert observed == immediate, 'Paint animation changed native editing/caret state'
                result['samples'].append({'capture_started_ns': shot_started,
                                          'capture_finished_ns': shot_finished,
                                          'capture_timestamps': timestamps,
                                          'timestamp_usable': timestamp_usable,
                                          'classification_clock': 'compositor',
                                          'guaranteed_within_duration': in_window,
                                          'guaranteed_after_duration': after_duration,
                                          'post_deadline_root_render': late_render,
                                          'native_state': observed,
                                          'protocol_end_offset': debug.stat().st_size})
                pending.append(image)
            # Full state/file scans and compositor identity/geometry IPC belong
            # outside the unchanged 120ms/300ms native pixel observation window.
            alive()
            assert window_geometry(window) == geometry, 'Native client moved during timed cursor sampling'
            crops = []
            for index, (item, image) in enumerate(zip(result['samples'], pending)):
                path = artifact(f'{label}-frame-{index:02d}', '.ppm')
                path.write_bytes(f"P6\n{image['width']} {image['height']}\n255\n".encode() + image['pixels'])
                row_bytes = image['width'] * 3
                crop = b''.join(image['pixels'][row * row_bytes + left * 3:row * row_bytes + right * 3]
                                for row in range(top, bottom))
                assert len(crop) == (right - left) * (bottom - top) * 3
                crop_path = artifact(f'{label}-frame-{index:02d}-caret-crop', '.ppm')
                crop_path.write_bytes(f'P6\n{right - left} {bottom - top}\n255\n'.encode() + crop)
                protocol_path = artifact(f'{label}-frame-{index:02d}', '.screencopy.wayland.log')
                protocol_path.write_bytes(image['capture_protocol'])
                item.update(capture=str(path), physical_pixels=(image['width'], image['height']),
                            pixel_sha256=hashlib.sha256(image['pixels']).hexdigest(),
                            crop_capture=str(crop_path), crop_physical_pixels=(right - left, bottom - top),
                            crop_sha256=hashlib.sha256(crop).hexdigest(),
                            capture_protocol_log=str(protocol_path))
                crops.append(crop)
            result['protocol_records'] = trace(offset)
            early = [index for index, item in enumerate(result['samples']) if item['guaranteed_within_duration']]
            settled_indices = [index for index, item in enumerate(result['samples']) if item['guaranteed_after_duration']]
            assert early, ('No actual native frame captured inside the configured trail duration', result)
            assert len(settled_indices) == 2, result
            settled_crop = crops[settled_indices[-1]]
            result['settled_crops_equal'] = all(crops[index] == settled_crop for index in settled_indices)
            result['within_duration_crop_differs_from_settled'] = any(crops[index] != settled_crop for index in early)
            result['all_crops_equal'] = all(crop == settled_crop for crop in crops)
            assert result['settled_crops_equal'], ('Native caret-local pixels did not settle after the finite duration', result)
            if expect_trail:
                assert result['within_duration_crop_differs_from_settled'], (
                    'Enabled Box/Trail showed no actual caret-local moving-to-settled pixel difference', result)
            else:
                assert result['all_crops_equal'], (
                    'Reduced motion/Effects Off did not keep the actual caret-local pixels static', result)
        else:
            result.update(photo(label))
        result['native_settled'] = state(cursor, cursor)
        assert result['native_settled'] == immediate, 'Paint animation changed native editing/caret state'
        result['protocol_records'] = trace(offset)
        return result

    def movement(label, *, sample=False, expect_trail=False):
        collapse_at_end()
        backward = move('ctrl+b', previous, label + '-backward', sample=sample, expect_trail=expect_trail)
        forward = move('ctrl+f', len(encoded), label + '-forward', sample=sample, expect_trail=expect_trail)
        return {'backward': backward, 'forward': forward}

    def output_at(scale):
        alive()
        active = [item for item in SESSION.ipc('get_outputs') if item['active']]
        assert len(active) == 1 and active[0]['name'] == 'HEADLESS-1', active
        output = active[0]
        pixels = tuple(round(value * scale) for value in (1440, 940))
        if (math.isclose(output['scale'], scale, abs_tol=1e-6)
                and (output['rect']['width'], output['rect']['height']) == (1440, 940)
                and (output['current_mode']['width'], output['current_mode']['height']) == pixels):
            return output
        return False

    def current(size, fullscreen):
        alive()
        matches = [(node, output) for node, output in tree_windows(SESSION.ipc('get_tree')) if node['id'] == window]
        assert len(matches) == 1, matches
        node, output = matches[0]
        rect = node['window_rect']
        if (output == 'HEADLESS-1' and node.get('visible')
                and (rect['width'], rect['height']) == size
                and node['fullscreen_mode'] == fullscreen):
            return node
        return False

    def restore_window():
        mode, rect = original_output['current_mode'], original_output['rect']
        refresh = mode.get('refresh', 60000) / 1000
        SESSION.ipc('command', f"output HEADLESS-1 mode --custom {mode['width']}x{mode['height']}@{refresh:g}Hz scale 1 position {rect['x']} {rect['y']}")
        SESSION.until('original private output scale and physical mode', lambda: output_at(1))
        restored_output = next(item for item in SESSION.ipc('get_outputs') if item['name'] == 'HEADLESS-1')
        assert restored_output['rect'] == original_output['rect'], (original_output, restored_output)
        offset = debug.stat().st_size
        SESSION.ipc('command', f'{criterion} fullscreen disable, floating enable, resize set width 1280 px height 800 px, focus')
        SESSION.until('restoration probe native window', lambda: current((1280, 800), 0))
        SESSION.until('restoration probe configure/ack/native buffer', lambda: protocol_evidence(debug, ids, offset, (1280, 800), 1))
        offset = debug.stat().st_size
        placement = f'{criterion} floating ' + ('enable' if original_floating else 'disable')
        if original_floating:
            placement += f", resize set width {original_geometry['WIDTH']} px height {original_geometry['HEIGHT']} px, move position {original_node['rect']['x']} px {original_node['rect']['y']} px"
        if original_fullscreen:
            placement += ', fullscreen enable' + (' global' if original_fullscreen == 2 else '')
        SESSION.ipc('command', placement + ', focus')
        SESSION.until('original fullscreen and logical native bounds', lambda: current((1440, 940), original_fullscreen))
        response = SESSION.until('restored native configure/ack/actual buffer', lambda: protocol_evidence(debug, ids, offset, (1440, 940), 1))
        assert window_geometry(window) == original_geometry, (original_geometry, window_geometry(window))
        assert (native_window(window)['floating'] in ('auto_on', 'user_on')) == original_floating
        return response

    database = sqlite3.connect(f'file:{db}?mode=ro', uri=True)
    geometry_changed = False
    try:
        original_saved = save_state(proc, prefs, db)
        state(initial['cursor'], initial['anchor'])
        original_history = database.execute('SELECT * FROM messages ORDER BY buffer,sort_time,seq').fetchall()
        original_drafts = database.execute('SELECT id,server,target,kind,draft FROM buffers ORDER BY id').fetchall()
        enabled_fx = appearance['fx'] if appearance['fx'] != 'off' else 'subtle'
        try:
            policy(enabled_fx, False, 'cursor-motion-enabled')
            for shape, effect in (('line', 'off'), ('box', 'off'), ('underline', 'off'),
                                  ('box', 'comet'), ('box', 'ribbon'), ('box', 'pulse'),
                                  ('box', 'trail')):
                saved = set_native_cursor_preferences(proc, window, prefs, db, args, dest,
                                                      shape=shape, effect=effect, duration=120, strength=.2)
                selected = saved['cursor'].copy()
                # Each real control operation is independently persisted by the
                # authoritative Settings helper; record its actual disk contents.
                report['selectedCursorPrefs'] = selected
                label = f'cursor-{shape}-{effect}'
                _, saved_path = disk(label, selected)
                report['cursor_controls'].append({'shape': shape, 'effect': effect,
                    'saved_cursor_preferences': selected, 'saved_settings': saved_path,
                    'editor': photo(label + '-actual-editor'),
                    'movement': movement(label, sample=effect == 'trail', expect_trail=effect == 'trail')})
            selected = report['selectedCursorPrefs']
            assert selected['shape'] == 'box' and selected['effect'] == 'trail', selected
            for fx, reduced, label in ((enabled_fx, True, 'cursor-reduced-motion'),
                                       ('off', False, 'cursor-effects-off')):
                item = policy(fx, reduced, label)
                item['movement'] = movement(label, sample=True)
                _, item['saved_settings'] = disk(label + '-after-keyboard', selected)
                report['suppression'].append(item)
        finally:
            # The ordinary success path closes every modal through its sticky
            # Close control. Do not inject Escape into a composer (reply cancel).
            policy(appearance['fx'], appearance['reduced_motion'], 'cursor-original-fx-restored')
        assert tomllib.loads(prefs.read_text())['appearance'] == appearance
        restore_selection()
        dpi_reference = state(initial['cursor'], initial['anchor'])
        geometry_changed = True
        for index, scale in enumerate((1, 1.25, 1.5, 2, 1), 1):
            label = f'cursor-dpi-{index:02d}-{scale:g}x'
            pixels = tuple(round(value * scale) for value in (1440, 940))
            offset = debug.stat().st_size
            SESSION.ipc('command', f'output HEADLESS-1 mode --custom {pixels[0]}x{pixels[1]}@60Hz scale {scale} position {original_output["rect"]["x"]} {original_output["rect"]["y"]}')
            output = SESSION.until(label + ' real output mode/scale', lambda: output_at(scale))
            SESSION.ipc('command', f'{criterion} fullscreen disable, floating enable, resize set width 1280 px height 800 px, focus')
            probe_node = SESSION.until(label + ' real floating probe geometry', lambda: current((1280, 800), 0))
            probe = SESSION.until(label + ' probe configure/ack/physical native buffer',
                                  lambda: protocol_evidence(debug, ids, offset, (1280, 800), scale))
            probe_state = state(initial['cursor'], initial['anchor'])
            offset = debug.stat().st_size
            SESSION.ipc('command', f'{criterion} fullscreen enable, focus')
            node = SESSION.until(label + ' exact 1440x940 fullscreen client', lambda: current((1440, 940), 1))
            response = SESSION.until(label + ' fullscreen configure/ack/physical native buffer',
                                     lambda: protocol_evidence(debug, ids, offset, (1440, 940), scale))
            native = state(initial['cursor'], initial['anchor'])
            # This is ONLY the public narrow IME rect, never hidden paint stamps.
            assert all(abs(a - b) <= 1 for a, b in zip(native['caret'], dpi_reference['caret'])), (dpi_reference, native, scale)
            _, saved_path = disk(label, selected)
            shot = photo(label)
            assert current((1440, 940), 1) and output_at(scale)
            report['dpi'].append({'stage': label, 'scale': scale, 'output': output,
                'probe_window': probe_node, 'probe_native_state': probe_state, 'probe_response': probe,
                'window': node, 'native_state': native, 'native_response': response,
                'saved_settings': saved_path, **shot})
        report['restored_native_response'] = restore_window()
        geometry_changed = False
        restored = restore_selection()
        final_saved = save_state(proc, prefs, db)
        assert_same_state('Cursor controls and same-window live DPI', original_saved, final_saved)
        assert database.execute('SELECT * FROM messages ORDER BY buffer,sort_time,seq').fetchall() == original_history, 'Cursor/DPI probe changed durable history'
        assert database.execute('SELECT id,server,target,kind,draft FROM buffers ORDER BY id').fetchall() == original_drafts, 'Cursor/DPI probe changed drafts or buffer identity'
        final, report['final_saved_settings'] = disk('cursor-dpi-restored', selected)
        assert final['appearance'] == appearance
        assert state(initial['cursor'], initial['anchor']) == restored
        alive()
        report.update(status='passed', native_after=restored, original_window_restored=True,
                      environment_unchanged=True, bindings_unchanged=True,
                      durable_history_unchanged=True, drafts_unchanged=True,
                      restored_capture=photo('cursor-dpi-original-window-restored'),
                      process_environment_sha256=hashlib.sha256(process_env).hexdigest())
        return report
    except BaseException as error:
        report.update(status='failed', error=f'{type(error).__name__}: {error}')
        raise
    finally:
        try:
            if geometry_changed and proc.poll() is None:
                restore_window()
            if report['status'] != 'passed' and proc.poll() is None:
                restore_selection()
        finally:
            database.close()
            assert dict(os.environ) == original_env and dict(SESSION.env) == original_session_env
            artifact('cursor-dpi-evidence', '.json').write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n')






def probe_menu_reachability(proc, window, preferences, database_path, args, destination):
    from collections import Counter
    import hashlib

    preferences, database_path, destination = map(Path, (preferences, database_path, destination))
    assert preferences.resolve().is_relative_to(Path(SESSION.env['XDG_CONFIG_HOME']).resolve())
    assert database_path.resolve().is_relative_to(Path(SESSION.env['XDG_DATA_HOME']).resolve())
    assert GUI_DEBUG[window][0] is proc and proc.poll() is None
    debug = GUI_DEBUG[window][1]
    ids = main_surface_ids(debug)
    assert ids[0] == GUI_DEBUG[window][2], (ids, GUI_DEBUG[window])
    press = re.compile(r'wl_keyboard[@#]\d+\.key\(\d+,\s*\d+,\s*\d+,\s*1\)')
    original_node = native_window(window)
    original_geometry = window_geometry(window)
    original_floating = original_node['floating'] in ('auto_on', 'user_on')
    original_fullscreen = original_node['fullscreen_mode']
    assert original_node['shell'] == 'xdg_shell' and original_node['border'] == 'none', original_node
    assert (original_geometry['WIDTH'], original_geometry['HEIGHT']) == tuple(args.size)
    outputs = [item for item in SESSION.ipc('get_outputs') if item['active']]
    assert len(outputs) == 1 and outputs[0]['name'] == 'HEADLESS-1', outputs
    original_output = outputs[0]
    assert original_output['scale'] == 1 and original_output['transform'] == 'normal', original_output
    original_env, original_session_env = dict(os.environ), dict(SESSION.env)
    process_env = Path(f'/proc/{proc.pid}/environ').read_bytes()
    identity = (window, proc.pid, original_node['app_id'])
    assert identity[2] == 'io.github.trevarj.blastirc', identity
    initial_input = native_input_state(window)
    assert initial_input and initial_input['caret'] is not None and initial_input['text'] is not None, initial_input

    def artifact(label, suffix):
        return destination.with_name(destination.stem + '-menu-' + label + suffix)

    def alive():
        assert proc.poll() is None, f'GUI exited during menu reachability: {proc.returncode}'
        actual = native_window(window)
        assert (actual['id'], actual['pid'], actual['app_id']) == identity, actual
        assert dict(os.environ) == original_env and dict(SESSION.env) == original_session_env
        assert Path(f'/proc/{proc.pid}/environ').read_bytes() == process_env

    def live_popups():
        live = set()
        for event in re.finditer(r'get_popup\(new id xdg_popup[@#](\d+),|xdg_popup[@#](\d+)\.destroy\(',
                                 debug.read_text(errors='replace')):
            if event[1]:
                live.add(event[1])
            else:
                live.discard(event[2])
        return sorted(live)

    def closed():
        alive()
        assert not live_popups(), ('Unexpected live native popup; refusing to hide it', live_popups())

    def fresh_save(before, label):
        def written():
            alive()
            try:
                stat = preferences.stat()
                if (stat.st_ino, stat.st_mtime_ns) == before:
                    return False
                return tomllib.loads(preferences.read_text())
            except (FileNotFoundError, tomllib.TOMLDecodeError):
                return False
        return SESSION.until(label + ': actual fresh private settings write', written)

    def persist(label):
        closed()
        return save_preferences(proc, preferences, window)

    def settings_save(label, *, scale=None):
        closed()
        zoom = tomllib.loads(preferences.read_text())['appearance']['scale']
        open_settings(window, preferences)
        framed_keys(window, 'shift+Tab')
        settings_category(window, zoom, 0)
        if scale is not None and not math.isclose(zoom, scale, abs_tol=1e-6):
            # Theme source, transparency, opacity, then Interface scale.
            stops = 4
            framed_keys(window, *(['Tab'] * stops))
            if scale == 2:
                framed_keys(window, 'End')
            else:
                steps = round((scale - .7) / .01)
                assert 0 <= steps <= 130 and math.isclose(.7 + steps * .01, scale, abs_tol=1e-5), scale
                pages, remainder = divmod(steps, 10)
                framed_keys(window, 'Home', *(['Page_Up'] * pages), *(['Right'] * remainder))
            framed_keys(window, *(['shift+Tab'] * stops))
        saved = save_settings_footer(window, saved_path=preferences, destination=destination,
                                     label='menu-' + label + '-actual-settings-save')
        if scale is not None:
            assert math.isclose(saved['appearance']['scale'], scale, abs_tol=1e-5), saved['appearance']
        framed_keys(window, 'ctrl+g')
        saved = persist(label + '-dialog-closed')
        assert native_input_rectangle(window) is not None, "Actual Close did not restore native Input focus"
        return saved

    closed()
    baseline = persist('baseline')
    original_zoom = baseline['appearance']['scale']
    assert math.isclose(original_zoom, args.zoom, abs_tol=1e-6), (original_zoom, args.zoom)
    workspace = baseline['workspace']
    assert not SETTINGS_OPEN.get(window, False), "Caller must dismiss Settings before menu input"
    assert workspace.get('native_dialog_rect'), 'Caller must establish original real Settings geometry first'
    initial_layout = read_layout(preferences)
    active = initial_layout["active_buffer"]
    assert_workspace(baseline, active, workspace["people_mode"])
    original_override = baseline['presence']['channels'].get(active)
    assert original_override in (None, 'all', 'smart', 'hidden'), baseline['presence']

    queries = {
        'messages': 'SELECT * FROM messages ORDER BY seq',
        'buffers': 'SELECT * FROM buffers ORDER BY id',
        'meta': 'SELECT * FROM meta ORDER BY key',
        'queued_sends': 'SELECT * FROM queued_sends ORDER BY seq',
        'closed_buffers': 'SELECT * FROM closed_buffers ORDER BY id',
        'reactions': 'SELECT * FROM reactions ORDER BY message,emoji,nick',
        'sqlite_sequence': 'SELECT * FROM sqlite_sequence ORDER BY name',
    }
    database = sqlite3.connect(f'file:{database_path}?mode=ro', uri=True)
    before_rows = {name: database.execute(query).fetchall() for name, query in queries.items()}
    active_row = database.execute('SELECT kind,draft FROM buffers WHERE id=?', (active,)).fetchone()
    assert active_row and active_row[0] == 'channel', (active, active_row)
    assert active_row[1] == initial_input['text'], (active_row, initial_input)
    assert database.execute("SELECT value FROM meta WHERE key='active_buffer'").fetchone() == (active,)
    assert not before_rows['queued_sends'] and database.execute(
        'SELECT count(*) FROM messages WHERE pending<>0').fetchone() == (0,), 'Caller must settle real sends first'

    # Native menu raster metrics constrain measured pixels, not preferred popup size.
    content_scale = 14 * 2 / 13
    row_height = 32 * content_scale
    padding = 6 * content_scale
    scrollbar_width = 6 * content_scale

    report = {
        'status': 'failed', 'window_identity': identity, 'protocol_log': str(debug),
        'root_surface': ids[0],
        'menu_contract': {'origin': 'Ui::Impl::paneMenu: visible channel header More',
                          'conditions': {'active_buffer': active, 'buffer_kind': 'channel',
                                         'buffer_exists': True, 'not_server': True},
                          'row_height': row_height, 'content_scale': content_scale, 'padding': padding},
        'semantic_tree': {
            'root': ['navigation', 'search', 'toggle-people', 'message-display',
                     'presence', 'history', 'topic', 'close-buffer'],
            'navigation': ['switch-buffer', 'jump-channel', 'toggle-navigation'],
            'message-display': ['message-style:bubbles', 'message-style:irc'],
            'presence': ['presence:global', 'presence:all', 'presence:smart', 'presence:hidden'],
            'history': ['load-history', 'mark-read']},
        'original_geometry': original_geometry, 'original_native_input': initial_input,
        'original_override': original_override, 'traversal': [], 'popups': [], 'captures': [],
        'sqlite_queries': queries, 'sqlite_before': {}, 'sqlite_after': {},
        'wire': {'observed': False, 'reason': 'No observer API; exact SQLite/native context evidence only'},
    }

    def digest(rows):
        return hashlib.sha256(repr(rows).encode()).hexdigest()

    def invariants(label):
        alive()
        after = {name: database.execute(query).fetchall() for name, query in queries.items()}
        for name in queries:
            assert after[name] == before_rows[name], (label + ' changed exact SQLite ' + name,
                                                     before_rows[name], after[name])
        return {name: {'rows': len(rows), 'sha256': digest(rows)} for name, rows in after.items()}

    report['sqlite_before'] = {name: {'rows': len(rows), 'sha256': digest(rows)}
                               for name, rows in before_rows.items()}

    def output_mode(size):
        mode, rect = original_output['current_mode'], original_output['rect']
        refresh = mode.get('refresh', 60000) / 1000
        SESSION.ipc('command', f"output HEADLESS-1 mode --custom {size[0]}x{size[1]}@{refresh:g}Hz scale 1 position {rect['x']} {rect['y']}")
        def ready():
            alive()
            actual = [item for item in SESSION.ipc('get_outputs') if item['active']]
            assert len(actual) == 1 and actual[0]['name'] == 'HEADLESS-1', actual
            output = actual[0]
            return (output if output['scale'] == 1 and output['transform'] == 'normal' and
                    (output['rect']['width'], output['rect']['height']) == tuple(size) and
                    (output['current_mode']['width'], output['current_mode']['height']) == tuple(size) else False)
        return SESSION.until('actual private output dimensions ' + repr(size), ready)

    def wait_root(size, offset):
        SESSION.until('same owned native client dimensions ' + repr(size), lambda:
                      (window_geometry(window)['WIDTH'], window_geometry(window)['HEIGHT']) == tuple(size))
        return SESSION.until('root configure/ack/correct non-NULL buffer commit', lambda:
                             protocol_evidence(debug, ids, offset, tuple(size), 1))

    def fit_popup(popup, offset, label):
        alive()
        # Obtain the actual fitted card from the owning wl_surface input region,
        # not preferred height, a copied shadow budget, or a fixed bottom point.
        regions, card = {}, None
        for line in debug.read_bytes()[offset:].decode(errors='replace').splitlines():
            match = re.search(r'create_region\(new id wl_region[@#](\d+)\)', line)
            if match:
                regions[match[1]] = []
            match = re.search(r'wl_region[@#](\d+)\.add\((-?\d+),\s*(-?\d+),\s*(\d+),\s*(\d+)\)', line)
            if match:
                assert match[1] in regions, line
                regions[match[1]].append(tuple(map(int, match.groups()[1:])))
            match = re.search(rf'wl_surface[@#]{popup["surface"]}\.set_input_region\(wl_region[@#](\d+)\)', line)
            if match:
                assert len(regions.get(match[1], [])) == 1, (line, regions)
                card = regions[match[1]][0]
        assert card is not None, 'Native popup did not publish its fitted card input region'
        px, py, pw, ph = popup['rect']
        x, y, width, height = card
        assert 0 <= x < x + width <= pw and 0 <= y < y + height <= ph, (popup, card)
        assert height > row_height, (card, row_height)
        popup.update(input_region=card, card=(x, y, width, height), log_offset=offset,
                     root_geometry=window_geometry(window))
        assert (popup['root_geometry']['WIDTH'], popup['root_geometry']['HEIGHT']) == compact_size
        report['popups'].append(dict(label=label, **popup))
        # Keep pointer hover away from all menu rows during keyboard traversal.
        move_pointer(window, 0, 0)
        return popup

    def open_more(label):
        closed()
        saved = persist(label + '-layout')
        assert_workspace(saved, active, workspace["people_mode"])
        layout = wait_layout(proc, preferences)
        assert layout["active_buffer"] == active, layout
        offset = debug.stat().st_size
        return fit_popup(open_title_menu(window, title_actions(layout, 2, channel=True)), offset, label)

    def branch(parent, index, label, symbol='Right'):
        offset = debug.stat().st_size
        if symbol == 'Right':
            child = open_native_submenu(window, parent, index)
        else:
            key('Home', *(['Down'] * index), symbol)
            child = await_native_popup(window, offset, parent=parent)
        assert child['parent_xdg'] == parent['xdg'], (parent, child)
        return fit_popup(child, offset, label)

    def navigate(popup, symbol):
        alive()
        family = live_popups()
        assert popup['popup'] in family, ('Unknown menu state', family)
        offset = debug.stat().st_size
        key(symbol)
        await_native_frame(window, offset, press, surface=popup['surface'])
        assert live_popups() == family, ('Navigation unexpectedly closed/replaced popup', symbol)
        return offset

    def crop(image, box):
        left, top, right, bottom = box
        return b''.join(image['pixels'][(y * image['width'] + left) * 3:
                                      (y * image['width'] + right) * 3] for y in range(top, bottom))

    def scopes(popup):
        x, y, width, height = popup['card']
        # Flat fill before toggle/icon/label; the old 12..20 band hits real icons.
        row_left = x + padding
        sample = (math.ceil(row_left + 2 * content_scale), y,
                  math.floor(row_left + 4 * content_scale), y + height)
        scrollbar = (math.floor(x + width - scrollbar_width), y, x + width, y + height)
        assert sample[0] < sample[2] < scrollbar[0] and sample[1] < sample[3], (sample, scrollbar)
        return sample, scrollbar

    def photograph(popup, label, *, retain=False):
        alive()
        geometry = window_geometry(window)
        assert geometry == popup['root_geometry'], (geometry, popup)
        assert popup['popup'] in live_popups(), ('Unknown menu state before native capture', live_popups())
        from PIL import Image
        path = capture_native_popup(window, popup, destination, 'menu-' + label)
        with Image.open(path) as captured:
            captured = captured.convert('RGB')
            image = {'width': captured.width, 'height': captured.height, 'pixels': captured.tobytes()}
        sample, scrollbar = scopes(popup)
        bar = crop(image, scrollbar)
        record = {'label': label, 'sourceRegion': geometry, 'sourceRegionHeight': geometry['HEIGHT'],
                  'popup_surface': popup['surface'], 'configured_popup_rect': popup['rect'],
                  'parent_xdg': popup['parent_xdg'], 'parent_rect': popup['parent_rect'],
                  'actual_input_region': popup['input_region'], 'scrollbar_scope': scrollbar,
                  'popup_png': str(path), 'scrollbar_rgb_sha256': hashlib.sha256(bar).hexdigest()}
        if retain:
            record['root_png'] = str(capture_window(window, destination, 'menu-' + label + '-root'))
        assert window_geometry(window) == geometry, 'Native popup capture moved its Root source region'
        report['captures'].append(record)
        return image, bar, record

    def highlight(image, popup, color):
        sample, _ = scopes(popup)
        left, top, right, bottom = sample
        ys = []
        for y in range(top, bottom):
            row = crop(image, (left, y, right, y + 1))
            if sum(row[x:x + 3] == color for x in range(0, len(row), 3)) >= .6 * (right - left):
                ys.append(y)
        assert ys and ys == list(range(ys[0], ys[-1] + 1)), ('Native highlight is absent/ambiguous/clipped', ys)
        assert row_height * .55 <= len(ys) <= row_height + 3, (ys[0], ys[-1], row_height)
        return (left, ys[0], right, ys[-1] + 1)

    def verify_highlight(image, popup, color):
        bounds = highlight(image, popup, color)
        assert popup['card'][1] <= bounds[1] < bounds[3] <= popup['card'][1] + popup['card'][3], bounds
        return bounds

    def icon_evidence(popup, first, second, first_bounds, second_bounds):
        from PIL import Image
        x, _, _, _ = popup['card']
        left = round(x + padding + 8 * content_scale)
        extent = round(12 * content_scale)
        masks, records, fills = [], [], []
        # Row zero is unselected in the Down frame; row one in the Home frame.
        for index, image, bounds in ((0, second, first_bounds), (1, first, second_bounds)):
            center = (bounds[1] + bounds[3]) / 2
            top = round(center - extent / 2)
            box = (left, top, left + extent, top + extent)
            flat_x = scopes(popup)[0][0]
            fill = crop(image, (flat_x, round(center), flat_x + 1, round(center) + 1))
            rgb = crop(image, box)
            mask = bytes(pixel != fill for pixel in (rgb[n:n + 3] for n in range(0, len(rgb), 3)))
            assert 5 < sum(mask) < len(mask) * .8, ('Missing/solid native icon raster', index, box)
            path = artifact(f'icon-{index:02d}', '.png')
            Image.frombytes('RGB', (extent, extent), rgb).save(path)
            records.append({'selectable_index': index, 'box': box, 'png': str(path),
                            'flat_rgb': list(fill), 'ink_pixels': sum(mask),
                            'mask_sha256': hashlib.sha256(mask).hexdigest()})
            masks.append(mask)
            fills.append(fill)
        assert fills[0] == fills[1], ('Icon crops do not share an unselected flat fill', fills)
        different = sum(a != b for a, b in zip(*masks))
        assert different > max(sum(mask) for mask in masks) * .15, 'Distinct native icons rendered identically'
        report['native_icon_rasters'] = {'icons': records, 'different_pixels': different}

    def visit_level(popup, entries, path):
        label = '-'.join(map(str, path)) or 'root'
        navigate(popup, 'Home')
        first, first_bar, first_record = photograph(popup, label + '-home', retain=True)
        previous = None
        bars = [first_bar]
        records = []
        for index, semantic in enumerate(entries):
            if index:
                navigate(popup, 'Down')
                image, bar, record = photograph(popup, label + f'-entry-{index:02d}')
            else:
                image, bar, record = first, first_bar, first_record
            bounds = verify_highlight(image, popup, hover)
            selected = crop(image, scopes(popup)[0])
            scrolled = bar != bars[-1]
            assert previous != selected or scrolled, (
                'Keyboard neither moved native highlight nor scrolled its viewport', path, index)
            record['measured_highlight'] = bounds
            report['traversal'].append({'path': [*path, index], 'expected_semantic_locator': semantic,
                                        'scrollbar_changed': scrolled, 'capture': record})
            records.append(record)
            bars.append(bar)
            previous = selected
        navigate(popup, 'End')
        end, end_bar, end_record = photograph(popup, label + '-end', retain=True)
        assert crop(end, scopes(popup)[0]) == previous, ('End failed to retain final row', path)
        assert end_bar == bars[-1], ('End changed final viewport', path)
        navigate(popup, 'Home')
        home, _, home_record = photograph(popup, label + '-home-restored')
        first_bounds = first_record['measured_highlight']
        home_bounds = verify_highlight(home, popup, hover)
        assert home_bounds[3] - home_bounds[1] == first_bounds[3] - first_bounds[1], (
            'Home changed the first selectable row highlight height', path, first_bounds, home_bounds)
        # Home reveals the first selectable row, not a preceding noninteractive
        # heading. Compare its label/chevron at native font-relative coordinates,
        # allowing only that legitimate scroll translation. Leading glyphs retain
        # their independent distinct native-icon raster proof above.
        toggle_slot = 16 if path in ([3], [4]) else 0
        left = math.ceil(popup['card'][0] + padding + (24 + toggle_slot) * content_scale)
        right = math.floor(scopes(popup)[1][0] - 8 * content_scale)
        first_label_box = (left, first_bounds[1], right, first_bounds[3])
        home_label_box = (left, home_bounds[1], right, home_bounds[3])
        first_label = crop(first, first_label_box)
        home_label = crop(home, home_label_box)
        assert sum(first_label[n:n + 3] != hover for n in range(0, len(first_label), 3)) > 5, (
            'First selectable row has no observable label/chevron ink', path, first_label_box)
        assert home_label == first_label, ('Home failed to restore the same selectable entry raster', path)
        home_record.update(measured_highlight=home_bounds, label_chevron_box=home_label_box,
                           label_chevron_sha256=hashlib.sha256(home_label).hexdigest())
        first_record.update(label_chevron_box=first_label_box,
                            label_chevron_sha256=hashlib.sha256(first_label).hexdigest())
        # Scrolling is required only when this actual level overflows its fitted card.
        overflow = any(bar != first_bar for bar in bars)
        if overflow:
            assert end_bar != first_bar, ('Overflow did not retain scrolled viewport', path)
        report.setdefault('levels', []).append({'path': path, 'overflow_observed': overflow,
                                                'first': first_record, 'last': end_record,
                                                'home_restored': home_record})
        return records

    def child_left(root, child):
        marker = debug.stat().st_size
        key('Left')
        SESSION.until('Left destroys only child popup', lambda: child['popup'] not in live_popups())
        assert live_popups() == [root['popup']], ('Left destroyed/replaced parent', live_popups())
        report.setdefault('keyboard_family', []).append({'key': 'Left', 'offset': marker,
                                                        'closed_child': child['popup'],
                                                        'retained_root': root['popup']})

    def outside_settings_click(root, child, native_before):
        point = shell_actions(read_layout(preferences))['settings']
        for member in (root, child):
            x, y, width, height = member['rect']
            assert not (x <= point[0] < x + width and y <= point[1] < y + height), (
                'Live Settings control is not outside this actual popup family', point, member)
        photograph(child, 'outside-settings-before', retain=True)
        marker = debug.stat().st_size
        move_pointer(window, *point)
        pointer_button(True)
        SESSION.until('outside real Settings press closes entire popup family', lambda: not live_popups())
        pointer_button(False)
        release = re.compile(r'wl_pointer[@#]\d+\.button\(\d+,\s*\d+,\s*272,\s*0\)')
        SESSION.until('outside click actual pointer release delivered', lambda:
                      release.search(debug.read_bytes()[marker:].decode(errors='replace')) is not None)
        saved = persist('outside-settings-click-swallowed')
        assert saved == compact, ('Outside click activated underlying live Settings control', compact, saved)
        SESSION.until('outside click retains exact native editing context', lambda:
                      native_input_state(window) == native_before)
        report['outside_click'] = {'point': point, 'live_control': 'Settings',
                                   'liveness_evidence': 'real Settings opened/saved at zoom two before traversal',
                                   'protocol_offset': marker, 'family': [root['popup'], child['popup']],
                                   'release_delivered': True, 'settings_unchanged': True,
                                   'native_input': native_input_state(window),
                                   'sqlite': invariants('outside Settings click swallowed'),
                                   'root_png': str(capture_window(window, destination, 'menu-outside-settings-after'))}

    try:
        settings_save('zoom-two', scale=2)
        marker = debug.stat().st_size
        toplevel = main_surface_ids(debug)[2]
        minimum_pattern = re.compile(rf'xdg_toplevel[@#]{toplevel}\.set_min_size\((\d+),\s*(\d+)\)')
        def declared_compact():
            requests = minimum_pattern.findall(debug.read_text(errors='replace'))
            if not requests or int(requests[-1][1]) < 960:
                return False
            return (max(720, int(requests[-1][0])), max(480, int(requests[-1][1])))
        compact_size = SESSION.until('actual scale-two native minimum for menu viewport', declared_compact)
        output_mode(compact_size)
        origin = original_output['rect']
        SESSION.ipc('command', f"[con_id={window}] fullscreen disable, floating enable, resize set width {compact_size[0]} px height {compact_size[1]} px, move position {origin['x']} px {origin['y']} px, focus")
        report['compact_root_protocol'] = wait_root(compact_size, marker if tuple(args.size) != compact_size else 0)
        report['compact_declared_minimum'] = compact_size
        compact = persist('compact')
        compact_workspace = compact['workspace'].copy()
        assert compact['appearance']['scale'] == 2, compact['appearance']
        compact_native = native_input_state(window)
        popup = open_more('traverse')
        navigate(popup, 'Home')
        first, first_bar, first_record = photograph(popup, 'first-home', retain=True)
        navigate(popup, 'Down')
        second, second_bar, second_record = photograph(popup, 'entry-01', retain=True)
        # Calibrate both complete native scopes: an antialiased boundary color
        # must not masquerade as the flat fill of an entire selected menu row.
        sample, _ = scopes(popup)
        left, top, right, bottom = sample
        histograms = ([], [])
        transitions = Counter()
        for y in range(top, bottom):
            rows = [crop(image, (left, y, right, y + 1)) for image in (first, second)]
            pixels = [[row[x:x + 3] for x in range(0, len(row), 3)] for row in rows]
            for index in (0, 1):
                histograms[index].append(Counter(pixels[index]))
            transitions.update((old, new) for old, new in zip(*pixels) if old != new)
        candidates = []
        colors = {old for old, new in transitions if transitions[(new, old)]}
        for color in colors:
            for index, histogram in enumerate(histograms):
                ys = [top + row for row, counts in enumerate(histogram)
                      if counts[color] >= .6 * (right - left)]
                if (not ys or ys != list(range(ys[0], ys[-1] + 1))
                        or not row_height * .55 <= len(ys) <= row_height + 3):
                    break
                # Rounded row corners may shorten this pre-icon flat-fill strip.
                # Its contiguous band, not a source row ordinal, is the evidence.
            else:
                candidates.append(color)
        assert len(candidates) == 1, (
            'Home/Down did not uniquely identify two complete native highlight bands',
            candidates, transitions.most_common(8))
        hover = candidates[0]
        first_record['measured_highlight'] = verify_highlight(first, popup, hover)
        second_record['measured_highlight'] = verify_highlight(second, popup, hover)
        assert first_record['measured_highlight'][1] < second_record['measured_highlight'][1]
        report['highlight_rgb'] = list(hover)
        icon_evidence(popup, first, second, first_record['measured_highlight'],
                      second_record['measured_highlight'])
        tree = report['semantic_tree']
        root_records = visit_level(popup, tree['root'], [])
        # Final root still genuinely overflows at this real geometry; do not
        # replace the former flat-list scrolling protection with metadata.
        assert report['levels'][0]['overflow_observed'], 'No real root scrollbar movement reached Close'
        report['last_item'] = {'expected_semantic_locator': 'close-buffer', 'focused': True, 'activated': False,
                               'capture': root_records[-1]}
        for index, semantic in ((0, 'navigation'), (3, 'message-display'), (4, 'presence'), (5, 'history')):
            child = branch(popup, index, semantic + '-right')
            assert set(live_popups()) == {popup['popup'], child['popup']}
            visit_level(child, tree[semantic], [index])
            child_left(popup, child)
        key('Left')  # Root Left is inert; it must not dismiss the root.
        photograph(popup, 'root-left-inert', retain=True)
        assert live_popups() == [popup['popup']]
        child = branch(popup, 0, 'navigation-return', symbol='Return')
        photograph(child, 'navigation-return-open', retain=True)
        report['keyboard_family'].append({'key': 'Return', 'opened_child': child['popup'],
                                           'parent_xdg': child['parent_xdg']})
        popup_keys(window, popup, 'Escape')
        closed()
        report['keyboard_family'].append({'key': 'Escape', 'closed_family': [popup['popup'], child['popup']]})
        invariants('all hierarchy entries visited without activation')
        assert persist('hierarchy-cancelled') == compact
        SESSION.until('hierarchy cancellation retains exact native editing context', lambda:
                      native_input_state(window) == compact_native)
        popup = open_more('outside-settings')
        child = branch(popup, 0, 'outside-settings-navigation')
        outside_settings_click(popup, child, compact_native)

        target_mode = 'hidden' if original_override != 'hidden' else 'smart'
        target_index = tree['presence'].index('presence:' + target_mode)
        popup = open_more('activate-presence')
        child = branch(popup, 4, 'activate-presence-child')
        navigate(child, 'Home')
        for _ in range(target_index):
            navigate(child, 'Down')
        selected_image, _, selected_record = photograph(child, 'presence-' + target_mode, retain=True)
        selected_record['measured_highlight'] = verify_highlight(selected_image, child, hover)
        stat = preferences.stat()
        popup_keys(window, popup, 'Return')
        closed()
        selected_saved = fresh_save((stat.st_ino, stat.st_mtime_ns), 'actual presence leaf activation')
        expected_presence = {**baseline['presence'], 'channels': {**baseline['presence']['channels'], active: target_mode}}
        assert selected_saved['presence'] == expected_presence, (selected_saved['presence'], expected_presence)
        selected_saved = settings_save('presence-leaf-saved')
        assert selected_saved['presence'] == expected_presence, selected_saved['presence']
        report['activation'] = {'path': [4, target_index], 'semantic': 'presence:' + target_mode,
                                'capture': selected_record, 'persisted_presence': selected_saved['presence'],
                                'settings_save': True, 'sqlite': invariants('presence leaf activation')}
        expected = {**compact, 'presence': expected_presence,
                    'workspace': {**compact_workspace, 'native_dialog_rect': selected_saved['workspace']['native_dialog_rect']}}
        assert selected_saved == expected, ('Presence activation/Save changed unrelated native settings', compact, selected_saved)

        popup = open_more('restore-presence')
        child = branch(popup, 4, 'restore-presence-child')
        restore_index = tree['presence'].index('presence:' + (original_override or 'global'))
        navigate(child, 'Home')
        for _ in range(restore_index):
            navigate(child, 'Down')
        restore_image, _, restore_record = photograph(child, 'original-presence-item-focused', retain=True)
        restore_record['measured_highlight'] = verify_highlight(restore_image, child, hover)
        stat = preferences.stat()
        popup_keys(window, popup, 'Return')
        closed()
        restored = fresh_save((stat.st_ino, stat.st_mtime_ns), 'original actual presence leaf activation')
        assert restored['presence'] == baseline['presence'], (restored['presence'], baseline['presence'])
        restored = settings_save('original-presence-saved')
        assert restored['presence'] == baseline['presence'], restored['presence']
        report['restoration'] = {'path': [4, restore_index], 'semantic': tree['presence'][restore_index],
                                 'capture': restore_record, 'persisted_presence': restored['presence'],
                                 'settings_save': True, 'sqlite': invariants('original presence restored')}

        closed()
        settings_save('restore-zoom-before-smaller-output', scale=original_zoom)
        marker = debug.stat().st_size
        mode = original_output['current_mode']
        output_mode((mode['width'], mode['height']))
        placement = f'[con_id={window}] floating ' + ('enable' if original_floating else 'disable')
        if original_floating:
            placement += f", resize set width {original_geometry['WIDTH']} px height {original_geometry['HEIGHT']} px, move position {original_node['rect']['x']} px {original_node['rect']['y']} px"
        if original_fullscreen:
            placement += ', fullscreen enable' + (' global' if original_fullscreen == 2 else '')
        SESSION.ipc('command', placement + ', focus')
        report['restored_root_protocol'] = wait_root(tuple(args.size), marker if tuple(args.size) != compact_size else 0)
        SESSION.until('exact original native window placement', lambda: window_geometry(window) == original_geometry)
        restored = settings_save('original-zoom-and-dialog-geometry', scale=original_zoom)
        assert restored == baseline, ('Menu probe changed exact original settings/workspace', baseline, restored)
        actual_output = next(item for item in SESSION.ipc('get_outputs') if item['name'] == 'HEADLESS-1')
        for field in ('rect', 'current_mode', 'scale', 'transform'):
            assert actual_output[field] == original_output[field], (field, original_output[field], actual_output[field])
        assert window_geometry(window) == original_geometry
        node = native_window(window)
        assert (node['floating'] in ('auto_on', 'user_on')) == original_floating
        assert node['fullscreen_mode'] == original_fullscreen
        SESSION.until('exact original native text/selection/caret after menu probe', lambda:
                      native_input_state(window) == initial_input)
        report['sqlite_after'] = invariants('complete menu reachability/restoration')
        report['restored_native_input'] = native_input_state(window)
        report['restored_workspace'] = restored['workspace']
        report['restored_geometry'] = window_geometry(window)
        report['restored_root_capture'] = str(capture_window(window, destination, 'menu-original-restored'))
        closed()
        expected_paths = [[index] for index in range(len(tree['root']))]
        for index, semantic in ((0, 'navigation'), (3, 'message-display'), (4, 'presence'), (5, 'history')):
            expected_paths.extend([index, leaf] for leaf in range(len(tree[semantic])))
        assert [entry['path'] for entry in report['traversal']] == expected_paths
        report['status'] = 'passed'
        print('Native More hierarchy reachability:', json.dumps({
            'visited_paths': expected_paths, 'activated': report['activation']['semantic'],
            'restored': report['restoration']['semantic'],
            'wire_observed': False, 'report': str(artifact('reachability', '.json'))}), flush=True)
        return report
    finally:
        database.close()
        report['live_popups_at_return_or_failure'] = live_popups()
        artifact('reachability', '.json').write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n')


def probe_shell_interactions(proc, window, preferences, database_path, args, destination, observer, *,
                             channel, channel_id, other_channel_id, query_target,
                             channel_messages, expected_messages):
    """Real drag-end writes, scaled rail raster, member lane and global read control."""
    initial = save_state(proc, preferences, database_path)
    native = native_input_state(window)
    assert native and native["caret"]
    report = {"drags": [], "captures": []}
    keys = ("server_rail_width", "channel_sidebar_width", "compact_channel_sidebar_width")
    original = tomllib.loads(preferences.read_text())["workspace"]
    defaults = (76, 250, 56)

    def retained():
        current = native_input_state(window)
        assert current and (current["text"], current["cursor"], current["anchor"]) == (
            native["text"], native["cursor"], native["anchor"]), (native, current)

    def drag(key, delta, *, rail=False):
        layout = read_layout(preferences)
        x, y, width, height = layout["rail" if rail else "channels"]["rect"]
        outer = layout["rail"]["rect"][0]
        point = (x + width + (outer - 1) / 2, y + height / 2)
        old = float(before_drag.get(key, defaults[keys.index(key)]))
        marker = GUI_DEBUG[window][1].stat().st_size
        move_pointer(window, *point)
        POINTER.button(True)
        move_pointer(window, point[0] + delta * args.zoom, point[1])
        POINTER.button(False)
        await_native_frame(window, marker,
            re.compile(r"wl_pointer[@#]\d+\.button\(\d+,\s*\d+,\s*272,\s*0\)"))

        def persisted():
            saved = tomllib.loads(preferences.read_text())["workspace"]
            value = float(saved.get(key, old))
            return saved if math.isclose(value, old + delta, abs_tol=.6) else False

        saved = SESSION.until("native drag-end persisted the actual logical width", persisted)
        for other in keys:
            if other != key:
                assert saved.get(other) == before_drag.get(other), (key, other, before_drag, saved)
        assert saved["native_navigation_mode"] == layout["navigation_intent"]
        retained()
        report["drags"].append({"key": key, "trigger": point, "logical_delta": delta,
                               "persisted": saved[key], "layout": read_layout(preferences)})

    def rail_ink():
        layout = read_layout(preferences)
        x, y, width, _ = layout["rail"]["rect"]
        move_pointer(window, 0, 0)
        sample = native_rgb_capture(window)
        row = round(y + 22 * args.zoom)
        left, right = round(x + 6 * args.zoom), round(x + width - 6 * args.zoom)
        colors = [sample["pixels"][(row * sample["width"] + column) * 3:
                                  (row * sample["width"] + column) * 3 + 3]
                  for column in range(left, right)]
        return sum(color != colors[0] for color in colors)

    set_navigation(proc, window, preferences, database_path, "expanded", "resize-expanded")
    before_drag = tomllib.loads(preferences.read_text())["workspace"]
    narrow_ink = rail_ink()
    drag(keys[0], 40, rail=True)
    wide_ink = rail_ink()
    assert wide_ink > narrow_ink + 20 * args.zoom, ("Rail avatar did not scale with actual width", narrow_ink, wide_ink)
    report["rail_avatar_raster_span"] = {"before": narrow_ink, "after": wide_ink}
    report["captures"].append(str(capture_window(window, destination, "shell-resized-rail-icons")))
    before_drag = tomllib.loads(preferences.read_text())["workspace"]
    drag(keys[1], 48)
    set_navigation(proc, window, preferences, database_path, "compact", "resize-compact")
    before_drag = tomllib.loads(preferences.read_text())["workspace"]
    drag(keys[2], 24)
    report["captures"].append(str(capture_window(window, destination, "shell-resized-compact-icons")))
    # Restore dimensions through real handles; explicit default-valued widths
    # remain durable intent, without editing preferences or adding a reset API.
    for key, rail, mode in ((keys[2], False, "compact"), (keys[1], False, "expanded"),
                            (keys[0], True, "expanded")):
        set_navigation(proc, window, preferences, database_path, mode, "resize-restore")
        before_drag = tomllib.loads(preferences.read_text())["workspace"]
        current = float(before_drag[key])
        target = float(original.get(key, defaults[keys.index(key)]))
        drag(key, target - current, rail=rail)
    set_navigation(proc, window, preferences, database_path, initial[0]["native_navigation_mode"], "resize-original-mode")

    query = initial[1][0]
    select_native_buffer(proc, window, preferences, database_path, args, channel_id)
    set_people(proc, window, preferences, database_path, "expanded")
    report["captures"].append(str(capture_window(window, destination, "shell-expanded-members-avatars")))
    set_people(proc, window, preferences, database_path, initial[0]["people_mode"])
    select_native_buffer(proc, window, preferences, database_path, args, query)
    retained()
    message = "Global read real-wire background marker"
    observer.send(f"PRIVMSG {channel} :{message}")
    added = channel_messages(message, query_target)
    expected_messages.extend(added)
    wait_messages(proc, database_path, added)
    database = sqlite3.connect(f"file:{database_path}?mode=ro", uri=True)
    try:
        deadline = time.monotonic() + 45
        while True:
            unread = database.execute("SELECT id,unread FROM buffers WHERE id IN (?,?) ORDER BY id",
                (channel_id, other_channel_id)).fetchall()
            if len(unread) == 2 and all(count > 0 for _, count in unread):
                break
            assert time.monotonic() < deadline, ("Background messages never became durably unread", unread)
            assert proc.poll() is None
            time.sleep(.025)
        history = database.execute("SELECT * FROM messages WHERE presence=0 ORDER BY seq").fetchall()
        rail_layout = read_layout(preferences)
        x, y, width, height = rail_layout["rail"]["rect"]
        extent, padding, gap = max(32, 36 * args.zoom), 6 * args.zoom, 6 * args.zoom
        # Conditional rail-mark-all-read is immediately ABOVE Add, then Settings.
        # Its locator follows allocation; durable read markers prove activation.
        footer_top = max(padding, height - padding - 3 * extent - 2 * gap)
        point = (x + width / 2, y + footer_top + extent / 2)
        add_point = (point[0], point[1] + extent + gap)
        assert point[1] < add_point[1]
        assert database.execute(
            "SELECT COUNT(*) FROM buffers WHERE kind<>'search' AND unread>0").fetchone()[0] > 0
        move_pointer(window, 0, 0)
        visible_rail = native_rgb_capture(window)
        marker = GUI_DEBUG[window][1].stat().st_size
        click(window, *point)
        await_native_frame(window, marker,
            re.compile(r"wl_pointer[@#]\d+\.button\(\d+,\s*\d+,\s*272,\s*0\)"))
        for buffer in (channel_id, other_channel_id):
            wait_read_marker(proc, database_path, buffer)
        wait_rows(proc, database_path, "SELECT COUNT(*) FROM buffers WHERE unread<>0", (), [(0,)])
        assert database.execute("SELECT * FROM messages WHERE presence=0 ORDER BY seq").fetchall() == history
        # Retiring the conditional row refocuses Add. Restore the original
        # Composer at its known end caret, without replacing its durable draft.
        cx, cy, cw, ch = native["caret"]
        click(window, cx + cw / 2, cy + ch / 2)
        command("end-of-buffer", window=window)
        move_pointer(window, 0, 0)
        hidden_rail = native_rgb_capture(window)
        left, top = round(x + padding), round(point[1] - extent / 2)
        right, bottom = round(x + width - padding), round(point[1] + extent / 2)
        def row_pixels(image):
            return b"".join(image["pixels"][(row * image["width"] + left) * 3:
                (row * image["width"] + right) * 3] for row in range(top, bottom))
        assert row_pixels(visible_rail) != row_pixels(hidden_rail), "Read rail row did not retire"
        assert database.execute(
            "SELECT COUNT(*) FROM buffers WHERE kind<>'search' AND unread>0").fetchone() == (0,)
        retained()
        report["global_read"] = {"trigger": point, "target": "rail-mark-all-read",
                                "above_add": add_point, "unread_before": unread,
                                "conditional_row_retired": True,
                                "covered_buffers": (channel_id, other_channel_id)}
        report["captures"].append(str(capture_window(window, destination, "shell-rail-global-read")))
    finally:
        database.close()
    command("end-of-buffer", window=window)
    retained()
    report["limits"] = "PNGs record real member/status rendering; native checks own glyph/color semantics. Drag-end disk persistence is observed; this scope does not restart."
    destination.with_name(destination.stem + "-shell-interactions.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def run_advanced_scope(*, proc, window, binary, env, screenshot, logs, preferences,
                       database_path, args, destination, observer, query_target,
                       query_id, other_query_id, own_nick, other_nick, channel,
                       channel_id, other_channel_id, cleanup, lab, receive,
                       channel_messages, expected_messages, input_method=None):
    """Run a complete native scope from one actual visible conversation."""
    scope = args.probe
    assert scope in ("settings", "composer", "cursor", "reading", "refresh", "menu", "shell"), scope
    assert scope == "menu" or (args.size == (1440, 940) and args.zoom == 1), args
    assert proc.poll() is None and GUI_DEBUG[window][0] is proc
    assert hasattr(observer, "frames") and hasattr(observer, "capabilities"), "Real WireObserver required"
    preferences, database_path = Path(preferences), Path(database_path)
    screenshot, destination = Path(screenshot), Path(destination)
    config = preferences.with_name("config.toml")
    wait_messages(proc, database_path, expected_messages)
    wait_read_marker(proc, database_path, channel_id)
    entry = save_state(proc, preferences, database_path)
    assert entry[1] == (channel_id,), entry
    assert_workspace({"workspace": entry[0]}, channel_id, entry[0]["people_mode"])
    observe_native_input(window, destination, "advanced-" + scope + "-empty-entry", "", 0, 0)
    probe_three_way_panels(proc, window, preferences, database_path, args, destination, channel_id)
    expected_messages.extend(probe_avy_cancel(
        proc, window, preferences, database_path, args, destination,
        lab=lab, cleanup=cleanup, own_nick=own_nick))
    layout = read_layout(preferences)
    marker = GUI_DEBUG[window][1].stat().st_size
    click(window, *shell_actions(layout)["palette"])
    await_native_frame(window, marker,
                       re.compile(r"wl_pointer[@#]\d+\.button\(\d+,\s*\d+,\s*272,\s*0\)"))
    capture_window(window, destination, "advanced-" + scope + "-topbar-command-palette")
    type_text("end-of-buffer")
    framed_keys(window, "Return")
    observe_native_input(window, destination, "advanced-" + scope + "-direct-palette-return", "", 0, 0)
    settings_before = tomllib.loads(preferences.read_text())
    profiles_before = tomllib.loads(config.read_text())
    expected_start = len(expected_messages)
    database = sqlite3.connect(f"file:{database_path}?mode=ro", uri=True)
    replacement_owned = False
    buffer_query = "SELECT id,server,target,kind,draft FROM buffers ORDER BY id"
    old_query = "SELECT * FROM messages WHERE presence=0 AND seq<=? ORDER BY seq"
    frontier = database.execute("SELECT COALESCE(MAX(seq),0) FROM messages").fetchone()[0]
    old_history = database.execute(old_query, (frontier,)).fetchall()
    entry_buffers = database.execute(buffer_query).fetchall()
    nonce = os.urandom(6).hex()
    opener_rows = []
    draft = ""
    active_buffer = channel_id

    def open_query(nick, identity, network):
        text = f"Astra {scope} {network} genuine query opener {nonce}"
        observer.send(f"PRIVMSG {nick} :{text}")
        expected_messages.append((identity, query_target, text, 0, 0))
        wait_messages(proc, database_path, expected_messages)
        rows = database.execute(
            "SELECT buffer,nick,text,own,pending,deleted,presence FROM messages WHERE text=?",
            (text,),
        ).fetchall()
        assert rows == [(identity, query_target, text, 0, 0, 0, 0)], rows
        opener_rows.extend(rows)

    try:
        if scope in ("settings", "composer", "refresh", "reading", "shell"):
            open_query(own_nick, query_id, "Local")
            if scope == "refresh":
                open_query(other_nick, other_query_id, "Other")
            select_native_buffer(proc, window, preferences, database_path, args, query_id)
            selected = save_state(proc, preferences, database_path)
            assert selected[1] == (query_id,), selected
            assert_workspace({"workspace": selected[0]}, query_id, entry[0]["people_mode"])
            wait_read_marker(proc, database_path, query_id)
            observe_native_input(window, destination, "advanced-" + scope + "-query-empty", "", 0, 0)
            active_buffer = query_id
        if scope == "refresh":
            select_native_buffer(proc, window, preferences, database_path, args, other_query_id)
            wait_read_marker(proc, database_path, other_query_id)
            select_native_buffer(proc, window, preferences, database_path, args, query_id)
        if scope in ("settings", "refresh", "shell"):
            draft = f"Astra {scope} original draft retained"
            type_text(draft)
            end = len(draft.encode("utf-8"))
            observe_native_input(window, destination, "advanced-" + scope + "-original-draft",
                                 draft, end, end)
        elif scope == "cursor":
            draft = "Astra café e\u0301 🙂中"
            seed_clipboard(draft, cleanup)
            framed_keys(window, "ctrl+v")
            end = len(draft.encode("utf-8"))
            # Observe completion of the genuine asynchronous paste BEFORE any
            # Settings/focus/save operation; these are UTF-8 bytes, not scalars.
            observe_native_input(window, destination, "advanced-cursor-mixed-unicode-draft",
                                 draft, end, end)
        if scope in ("settings", "cursor", "menu"):
            # The first real dialog records native_dialog_rect. Establish that
            # genuine original-bounds geometry before exact before/after checks.
            open_settings(window, preferences)
            framed_keys(window, "shift+Tab")
            save_settings_footer(window, saved_path=preferences)
            framed_keys(window, "shift+Tab", "Return")  # Sticky Close from navigation.
            SETTINGS_OPEN[window] = False
            save_state(proc, preferences, database_path)
            wait_layout(proc, preferences, predicate=lambda layout: "settings" not in layout)
        baseline = save_state(proc, preferences, database_path)
        baseline_settings = tomllib.loads(preferences.read_text())
        baseline_buffers = database.execute(buffer_query).fetchall()
        assert baseline[1] == (active_buffer,), baseline
        assert_workspace({"workspace": baseline[0]}, active_buffer, baseline[0]["people_mode"])
        wait_rows(proc, database_path, "SELECT draft FROM buffers WHERE id=?",
                  (active_buffer,), [(draft,)])
        end = len(draft.encode("utf-8"))
        observe_native_input(window, destination, "advanced-" + scope + "-ready", draft, end, end)
        changed_read_buffers = set()
        evidence = None

        if scope == "settings":
            rows = probe_settings_categories(
                proc, window, preferences, config, database_path, args, destination,
                observer=observer, query_target=query_target, query_id=query_id,
                own_nick=own_nick, cleanup=cleanup, lab=lab, receive=receive)
            assert len(rows) == 2 and all(len(row) == 7 for row in rows), rows
            for buffer, identity, nick, text, own, pending, deleted in rows:
                assert buffer == query_id and identity and pending == 0 and deleted == 0, rows
                expected_messages.append((buffer, nick, text, own, pending))
            changed_read_buffers.add(query_id)
        elif scope == "composer":
            expected_messages.extend(probe_native_formatting(
                proc, window, preferences, database_path, args, destination,
                query_id=query_id, query_target=query_target, own_nick=own_nick, receive=receive))
            expected_messages.extend(probe_native_emoji(
                proc, window, preferences, database_path, args, destination,
                query_id=query_id, query_target=query_target, own_nick=own_nick, receive=receive))
            rows = probe_reply_paste(
                proc, window, preferences, database_path, args, destination, observer,
                query_id=query_id, query_target=query_target, own_nick=own_nick,
                cleanup=cleanup, lab=lab, receive=receive)
            # The helper reports the actual reply-policy/msgid gate explicitly
            # when unavailable, and still requires all literal bulk-paste paths.
            assert len(rows) in (7, 8) and all(len(row) == 5 for row in rows), rows
            expected_messages.extend(rows)
            expected_messages.extend(probe_reaction_picker(
                proc, window, preferences, database_path, args, destination, observer,
                query_id=query_id, query_target=query_target, own_nick=own_nick,
                lab=lab, receive=receive))
            expected_messages.extend(probe_keyboard_reply(
                proc, window, preferences, database_path, args, destination, observer,
                query_id=query_id, query_target=query_target, own_nick=own_nick,
                cleanup=cleanup, lab=lab, receive=receive))
            changed_read_buffers.add(query_id)
        elif scope == "refresh":
            probe_flat_refresh(
                proc, window, preferences, database_path, args, destination,
                observer=observer, query_target=query_target, query_id=query_id,
                other_query_id=other_query_id, cleanup=cleanup, lab=lab)
        elif scope == "reading":
            evidence = probe_reading_anchor(
                proc, window, preferences, database_path, args, destination, observer,
                channel=channel, channel_id=channel_id, other_channel_id=other_channel_id,
                query_target=query_target, channel_messages=channel_messages,
                expected_messages=expected_messages)
            evidence["transcript_surfaces"] = probe_transcript_surfaces(
                proc, window, preferences, database_path, args, destination, observer,
                channel=channel, channel_id=channel_id, other_channel_id=other_channel_id,
                query_target=query_target, query_id=query_id, own_nick=own_nick,
                channel_messages=channel_messages, expected_messages=expected_messages,
                cleanup=cleanup, lab=lab, receive=receive)
            changed_read_buffers.update((channel_id, other_channel_id, query_id))
        elif scope == "shell":
            evidence = probe_shell_interactions(
                proc, window, preferences, database_path, args, destination, observer,
                channel=channel, channel_id=channel_id, other_channel_id=other_channel_id,
                query_target=query_target, channel_messages=channel_messages,
                expected_messages=expected_messages)
            changed_read_buffers.update(row[0] for row in baseline[2])
        elif scope == "menu":
            evidence = probe_menu_reachability(
                proc, window, preferences, database_path, args, destination)
        elif scope == "cursor":
            evidence = probe_cursor_controls_dpi(
                proc, window, preferences, database_path, args, destination)
            before_cursor = evidence["beforeCursorPrefs"]
            selected_cursor = evidence["selectedCursorPrefs"]
            assert before_cursor == baseline_settings["cursor"], (before_cursor, baseline_settings["cursor"])
            assert selected_cursor["shape"] == "box" and selected_cursor["effect"] == "trail", selected_cursor
            assert tomllib.loads(preferences.read_text())["cursor"] == selected_cursor
            cursor_history = database.execute(
                "SELECT * FROM messages WHERE presence=0 ORDER BY seq").fetchall()
            before_restart = save_state(proc, preferences, database_path)
            assert_same_state("Cursor probe before actual restart", baseline, before_restart)
            target = destination.with_name(destination.stem + "-cursor-before-restart.bmp")
            stdout, stderr = finish_gui(proc, window, screenshot, target, args.size)
            logs.append(("advanced-cursor-before-restart", stdout, stderr))
            departed = {own_nick, other_nick}
            while departed:
                _, _, prefix = observer.wait(
                    lambda actual, params, prefix: actual == "QUIT" and
                    prefix.split("!", 1)[0] in departed, timeout=15)
                departed.remove(prefix.split("!", 1)[0])
            screenshot = screenshot.with_name(screenshot.stem + "-cursor-restart.bmp")
            proc, window = launch_gui(binary, screenshot, env, args.size, logs)
            replacement_owned = True
            wait_joins(observer, channel, (own_nick, other_nick))
            restarted = save_state(proc, preferences, database_path)
            assert_same_state("Cursor actual same-profile restart", before_restart, restarted)
            restarted_settings = tomllib.loads(preferences.read_text())
            assert restarted_settings["cursor"] == selected_cursor, restarted_settings["cursor"]
            assert {key: value for key, value in restarted_settings.items() if key != "cursor"} == {
                key: value for key, value in baseline_settings.items() if key != "cursor"
            }, "Actual cursor restart changed non-cursor preferences/workspace"
            assert database.execute(buffer_query).fetchall() == baseline_buffers, "Restart changed original drafts"
            assert database.execute(
                "SELECT * FROM messages WHERE presence=0 ORDER BY seq").fetchall() == cursor_history, "Restart changed non-presence history"
            # Draft bytes and preferences are stored; cursor/anchor/undo stacks
            # are NOT. Position explicitly, then capture the restarted editor.
            command("end-of-buffer", window=window)
            editor = observe_native_input(
                window, destination, "cursor-restart-actual-editor", draft, end, end)
            open_settings(window, preferences)
            framed_keys(window, "shift+Tab")
            settings_category(window, args.zoom, 0)
            shape_stop = 12
            framed_keys(window, *(["Tab"] * shape_stop))
            controls = []
            # Appearance's cursor Selects initialize from the
            # reloaded model; SDK SelectDropdownPopup sets its checkmark and
            # initialHighlight from selectedIndex. Do not choose/rewrite them.
            for label in ("shape", "effect"):
                focused = capture_window(window, destination, "cursor-restart-selected-" + label)
                popup = open_native_popup(window, lambda: key("Return"))
                opened = capture_window(window, destination, "cursor-restart-selected-" + label + "-popup")
                controls.append({"control": label, "popup": popup,
                                 "captures": [str(focused), str(opened)]})
                popup_keys(window, popup, "Escape", root_changed=False)
                if label == "shape":
                    framed_keys(window, "Tab")
            framed_keys(window, "Tab", "Tab", "Tab", "Return")  # Try in conversation.
            SETTINGS_OPEN[window] = False
            saved_controls = save_state(proc, preferences, database_path)
            assert_same_state("Restarted selected cursor controls inspection", restarted, saved_controls)
            assert tomllib.loads(preferences.read_text())["cursor"] == selected_cursor
            restored = set_native_cursor_preferences(
                proc, window, preferences, database_path, args, destination,
                shape=before_cursor["shape"], effect=before_cursor["effect"],
                duration=before_cursor["trail_duration_ms"], strength=before_cursor["trail_strength"])
            assert restored["cursor"] == before_cursor, restored["cursor"]
            evidence = {"beforeCursorPrefs": before_cursor,
                        "selectedCursorPrefs": selected_cursor,
                        "same_profile_restart": True, "restarted_pid": proc.pid,
                        "restarted_editor": editor, "restarted_selected_controls": controls,
                        "original_cursor_preferences_restored": True,
                        "limits": "Only stored draft/history/preferences persistence is asserted; "
                                  "the restarted caret was explicitly moved to end-of-buffer. "
                                  "No cursor/selection/undo-stack restart persistence claim."}

        wait_messages(proc, database_path, expected_messages)
        if active_buffer in changed_read_buffers:
            wait_read_marker(proc, database_path, active_buffer)
        final = save_state(proc, preferences, database_path)
        assert final[0] == baseline[0] and final[1] == baseline[1], (baseline, final)
        assert database.execute(buffer_query).fetchall() == baseline_buffers, "Advanced scope changed original drafts/buffer identities"
        if not changed_read_buffers:
            assert_same_state("Advanced " + scope + " restored exact native state", baseline, final)
        else:
            protected = [row for row in baseline[2] if row[0] not in changed_read_buffers]
            actual = [row for row in final[2] if row[0] not in changed_read_buffers]
            assert actual == protected, ("Scope changed an unrelated read marker/unread/draft", protected, actual)
        final_settings = tomllib.loads(preferences.read_text())
        assert saved_workspace_intent(final_settings) == saved_workspace_intent(baseline_settings), (
            "Advanced scope did not restore semantic workspace state and non-workspace preferences",
            baseline_settings, final_settings)
        assert {key: value for key, value in final_settings.items() if key != "workspace"} == {
            key: value for key, value in settings_before.items() if key != "workspace"
        }, "Scope setup changed original effects, cursor, bindings or other preferences"
        assert tomllib.loads(config.read_text()) == profiles_before, "Advanced scope changed original profile bindings"
        assert database.execute(old_query, (frontier,)).fetchall() == old_history, "Advanced scope mutated/deleted old non-presence history"
        new_rows = database.execute(
            "SELECT buffer,nick,text,own,pending,deleted FROM messages "
            "WHERE presence=0 AND seq>? ORDER BY buffer,nick,text,own,pending,deleted",
            (frontier,),
        ).fetchall()
        expected_new = expected_messages[expected_start:]
        assert all(len(row) == 5 for row in expected_messages), expected_messages
        assert new_rows == sorted((*row, 0) for row in expected_new), (
            "Unexpected/missing durable rows outside exact genuine-wire expectations", expected_new, new_rows)
        # Setup may add genuine query buffers, but may not alter any preexisting
        # identity or draft except the explicitly seeded cursor channel draft.
        retained = {row[0]: row for row in baseline_buffers}
        for row in entry_buffers:
            expected = (*row[:4], draft) if scope == "cursor" and row[0] == channel_id else row
            assert retained[row[0]] == expected, (expected, retained[row[0]])
        observe_native_input(window, destination, "advanced-" + scope + "-final-draft",
                             draft, end, end)
        print("Native focused advanced scope:", lab._redact(json.dumps({
            "scope": scope, "genuine_query_openers": opener_rows,
            "exact_new_nonpresence_rows": expected_new,
            "old_nonpresence_rows_unchanged": len(old_history),
            "original_drafts_and_semantic_workspace_preserved": True,
            "non_workspace_preferences_and_profile_bindings_restored": True,
            "evidence": evidence,
        }, ensure_ascii=False)), flush=True)
        return proc, window, screenshot, expected_messages
    except BaseException:
        # Until return, only this frame knows a replacement process/window. Do
        # not leave that actual app alive for the caller's stale-process cleanup.
        if replacement_owned:
            stop_group(proc)
            stdout, stderr = collect_gui(proc)
            logs.append(("advanced-cursor-restart-failure", stdout, stderr))
        raise
    finally:
        database.close()


def run():
    global SESSION, POINTER, CHANNEL_ID, QUERY_ID
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", nargs="?", type=Path, default=ROOT / "build-native/blastirc")
    parser.add_argument("--message-style", choices=("bubbles", "irc"), default="bubbles",
                        help="initial per-conversation display; both modes are restarted in the full smoke")
    parser.add_argument("--zoom", type=float, default=1,
                        help="user zoom in private test preferences (.7..2; default: 1)")
    parser.add_argument("--size", type=window_size, default=(1440, 940), metavar="WIDTHxHEIGHT",
                        help="native Wayland logical dimensions, at least 720x480 (default: 1440x940)")
    parser.add_argument("--palette", type=Path, metavar="PATH",
                        help="copy a real Noctalia palette unchanged into the private test profile")
    parser.add_argument("--reduced-motion", action="store_true",
                        help="exercise the native reduced-motion preference")
    parser.add_argument("--probes", action="store_true",
                        help="full workflow plus native IME, reply/bulk paste, Settings and refresh probes")
    parser.add_argument("--layout-only", action="store_true",
                        help="focused native one-line/Send/history surface and real-wire check, not the full workflow")
    parser.add_argument("--ime-only", action="store_true",
                        help="focused genuine compositor preedit/partial-commit/Unicode and real-wire check")
    parser.add_argument("--raw-only", action="store_true",
                        help="real native keyboard/clipboard/edit/IRC checks with NO input-method client or metadata")
    parser.add_argument("--probe", choices=("settings", "composer", "cursor", "reading", "refresh", "menu", "shell"),
                        help="one complete real native advanced scope without replaying the full workflow")
    args = parser.parse_args()
    if not math.isfinite(args.zoom) or not .7 <= args.zoom <= 2:
        parser.error("--zoom must be finite and between .7 and 2")
    if (args.probes or args.probe) and (args.size != (1440, 940) or args.zoom != 1):
        parser.error("--probes and --probe require the representative 1440x940, zoom 1 layout")
    if sum((args.layout_only, args.ime_only, args.raw_only, args.probes, bool(args.probe))) > 1:
        parser.error("--layout-only, --ime-only, --raw-only, --probes and --probe are distinct verification scopes")
    binary = args.binary.resolve()
    for executable in ("sway", "swaymsg", "wtype", "wl-copy", "wl-paste", "grim", "pkg-config", "tesseract", str(binary)):
        if shutil.which(executable) is None:
            parser.error(f"{executable!r} is missing; invoke inside nix develop")
    with fixtures.LocalTopology() as lab, tempfile.TemporaryDirectory(prefix="blastirc-gui-") as temporary, ExitStack() as cleanup:
        tmp = Path(temporary)
        endpoint = lab.endpoints["direct"]
        other_endpoint = lab.endpoints["peer"]
        channel = endpoint["channels"][0]
        config = tmp / "config/blastirc"
        channel_id = f"Local/{channel}"
        CHANNEL_ID = channel_id
        other_channel_id = f"Other/{channel}"
        database_path = tmp / "data/blastirc/state.sqlite3"
        config.mkdir(parents=True)
        # json.dumps is valid for these TOML basic strings.
        q = json.dumps
        profile_text = "schema_version = 1\n"
        for network, identity, profile in (
                ("Local", "default", endpoint), ("Other", "other", other_endpoint)):
            profile_text += (
                f'[[identity]]\nname = {q(identity)}\n'
                f'nick = {q(profile["nick"])}\nusername = {q(profile["username"])}\n'
                f'realname = "GUI smoke"\npassword = {q(profile["password"])}\n'
                f'[[server]]\nname = {q(network)}\n'
                # Profiles use "address", not the real fixture endpoint's "host".
                f'address = {q(profile["host"])}\nport = {profile["port"]}\n'
                f'tls = {q(profile["tls"])}\nidentity = {q(identity)}\n'
                f'sasl = {q(profile["sasl"])}\npassword = {q(profile["server_password"])}\n'
                f'bouncer = {q(profile["bouncer"])}\nchannels = {q(profile["channels"])}\n'
            )
        (config / "config.toml").write_text(profile_text)
        (config / "config.toml").chmod(0o600)
        preferences = config / "settings.toml"
        preferences.write_text(
            f'[appearance]\nscale = {args.zoom}\ntheme_source = "auto"\n'
            f'reduced_motion = {str(args.reduced_motion).lower()}\n'
            '[presentation]\n'
            f'irc_channels = {q([channel_id, "Local/guiobserver"] if args.message_style == "irc" else [])}\n'
        )
        preferences.chmod(0o600)
        palette_bytes = None
        if args.palette is not None:
            palette = config / "noctalia.toml"
            palette_bytes = args.palette.read_bytes()
            palette.write_bytes(palette_bytes)
            palette.chmod(0o600)
        env = dict(os.environ, HOME=str(tmp / "home"), XDG_CONFIG_HOME=str(tmp / "config"),
                   XDG_DATA_HOME=str(tmp / "data"), XDG_STATE_HOME=str(tmp / "state"),
                   XDG_CACHE_HOME=str(tmp / "cache"), SSL_CERT_FILE=str(lab.ca_file),
                   LIBGL_ALWAYS_SOFTWARE=str(1))
        for name in ("home", "data", "state", "cache"):
            (tmp / name).mkdir(mode=0o700)
        SESSION = cleanup.enter_context(HeadlessSway(
            tmp / "runtime", tmp / "compositor", env,
            outputs=(("HEADLESS-1", *args.size, 0, 0, 1),)))
        env = SESSION.env
        POINTER = cleanup.enter_context(VirtualSeat(
            Path(env["XDG_RUNTIME_DIR"]) / env["WAYLAND_DISPLAY"]))
        SESSION.until("owned native pointer and keyboard devices", lambda:
                      {"pointer", "keyboard"} <= {
                          device.get("type") for device in SESSION.ipc("get_inputs")})
        # A normal desktop has an IM provider before toplevels request text
        # input. Keep this real service alive for ordinary metadata consumers;
        # the late-registration regression and raw-no-IME scope remain distinct.
        input_method = None
        if not args.ime_only and not args.raw_only:
            input_method = cleanup.enter_context(NativeInputMethod(POINTER))
        observer_endpoint = dict(endpoint, nick="guiobserver", username="guiobserver")
        query_target = observer_endpoint["nick"]
        query_id = f"Local/{query_target}"
        QUERY_ID = query_id
        own_nick = endpoint["nick"]
        other_nick = other_endpoint["nick"]
        other_query_id = f"Other/{query_target}"

        def channel_messages(text, nick):
            return [(channel_id, nick, text, int(nick == own_nick), 0),
                    (other_channel_id, nick, text, int(nick == other_nick), 0)]

        observer_type = WireObserver if args.probes or args.probe else fixtures.BootstrapClient
        with observer_type(observer_endpoint, lab.ca_file, authenticate=False) as observer:
            def receive(command, params, nick):
                return receive_until(observer, command, params, nick, redact=lab._redact)

            server_buffer = "Local/*server*"

            def server_sequence():
                database = sqlite3.connect(f"file:{database_path}?mode=ro", uri=True)
                try:
                    return database.execute(
                        "SELECT COALESCE(MAX(seq),0) FROM messages WHERE buffer=?",
                        (server_buffer,),
                    ).fetchone()[0]
                finally:
                    database.close()

            def disconnect_local(proc):
                sequence = server_sequence()
                command("disconnect")
                observer.wait(lambda actual, params, prefix:
                              actual == "QUIT" and prefix.split("!", 1)[0] == own_nick, timeout=15)
                wait_rows(proc, database_path,
                          "SELECT EXISTS(SELECT 1 FROM messages WHERE buffer=? AND seq>? AND text=?)",
                          (server_buffer, sequence, "Disconnected"), [(1,)])

            def default_navigation(proc, window, active_buffer, label):
                layout = set_navigation(proc, window, preferences, database_path, "expanded", label + "-expanded")
                live = save_state(proc, preferences, database_path)
                assert_workspace({"workspace": live[0]}, active_buffer, "hidden")
                assert layout["navigation_intent"] == "expanded", layout
                return layout, sidebar_expanded(layout)

            observer.send("JOIN " + channel)
            receive("JOIN", [channel], observer_endpoint["nick"])
            observer.send("CAP LS 302")
            available = set()
            while True:
                _, params, _ = observer.wait(lambda actual, params, prefix:
                                             actual == "CAP" and "LS" in params)
                available.update(cap.split("=", 1)[0] for cap in params[-1].split())
                if len(params) < 4 or params[-2] != "*":
                    break
            redaction = {"message-tags", "draft/message-redaction"} <= available
            if redaction:
                observer.send("CAP REQ :message-tags draft/message-redaction")
                observer.wait(lambda actual, params, prefix: actual == "CAP" and "ACK" in params)
            screenshot = tmp / "workspace.bmp"
            destination = Path(os.environ.get("BLASTIRC_SMOKE_SCREENSHOT", "/tmp/blastirc-gui-smoke.bmp"))
            if palette_bytes is not None:
                palette_artifact = destination.with_suffix(".palette.toml")
                palette_artifact.write_bytes(palette_bytes)
                print("Native palette provenance:", json.dumps({
                    "artifact": str(palette_artifact),
                    "sha256": hashlib.sha256(palette_bytes).hexdigest(),
                }), flush=True)
            proc = None
            stdout = stderr = None
            failed = True
            window = None
            logs = []
            try:
                proc, window = launch_gui(binary, screenshot, env, args.size, logs)
                print(f"GUI window={window} Wayland={env['WAYLAND_DISPLAY']} "
                      f"zoom={args.zoom:g} size={args.size[0]}x{args.size[1]}", flush=True)
                wait_joins(observer, channel, (own_nick, other_nick))
                switch_buffer(channel)
                if args.probes:
                    framed_keys(window, "ctrl+x")
                    observe_prefix_caret(window, destination, args.reduced_motion)
                else:
                    key("ctrl+x")
                    time.sleep(2.5)  # Emacs prefixes survive a deliberate pause.
                key("b")
                type_text(channel + " Local")
                key("Return")
                initial = save_state(proc, preferences, database_path)
                assert_workspace({"workspace": initial[0]}, channel_id, "hidden")
                assert initial[1] == (channel_id,), initial
                channel_incoming = (
                    "\x02Incoming geometry check\x02 — "
                    "A real peer message wraps across this pane while preserving readable spacing, "
                    "bold text and emoji ✨. The same conversation remains visible through "
                    "font, scaling and navigation changes; these words exercise multi-line rows "
                    "without replacing the shared chat buffer or its editor."
                )
                observer.send(f"PRIVMSG {channel} :{channel_incoming}")
                expected_messages = channel_messages(channel_incoming, query_target)
                wait_messages(proc, database_path, expected_messages)
                save_state(proc, preferences, database_path)
                capture_window(window, destination, "channel-" + args.message_style + "-initial")
                if args.probe:
                    proc, window, screenshot, expected_messages = run_advanced_scope(
                        proc=proc, window=window, binary=binary, env=env,
                        screenshot=screenshot, logs=logs, preferences=preferences,
                        database_path=database_path, args=args, destination=destination,
                        observer=observer, query_target=query_target, query_id=query_id,
                        other_query_id=other_query_id, own_nick=own_nick, other_nick=other_nick,
                        channel=channel, channel_id=channel_id, other_channel_id=other_channel_id,
                        cleanup=cleanup, lab=lab, receive=receive, channel_messages=channel_messages,
                        expected_messages=expected_messages, input_method=input_method)
                    stdout, stderr = finish_gui(proc, window, screenshot, destination, args.size)
                    logs.append(("advanced-" + args.probe, stdout, stderr))
                    destination.with_suffix(".wayland.log").write_text(lab._redact(stderr))
                    failed = False
                    print(f"Focused native {args.probe} scope passed with exact real-wire/durable expectations.", flush=True)
                    return
                if args.probes or args.ime_only or not (args.layout_only or args.raw_only):
                    ime_text = probe_real_ime(window, destination, cleanup,
                                              read_layout(preferences), args.zoom, method=input_method)
                    key("Return")
                    receive("PRIVMSG", [channel, ime_text], own_nick)
                    expected_messages.extend(channel_messages(ime_text, own_nick))
                    if args.ime_only:
                        wait_messages(proc, database_path, expected_messages)
                        save_state(proc, preferences, database_path)
                        wait_rows(proc, database_path, "SELECT draft FROM buffers WHERE id=?", (channel_id,), [("",)])
                        capture_window(window, destination, "real-ime-delivered")
                        stdout, stderr = finish_gui(proc, window, screenshot, destination, args.size)
                        logs.append(("focused-real-ime", stdout, stderr))
                        failed = False
                        print("Genuine compositor preedit/partial-commit/Unicode and exact IRC delivery passed.", flush=True)
                        return
                seed_clipboard("abcXYZ", cleanup)
                key("ctrl+v")
                save_state(proc, preferences, database_path)
                wait_rows(proc, database_path, "SELECT draft FROM buffers WHERE id=?",
                          (channel_id,), [("abcXYZ",)])
                if args.layout_only:
                    for text in ("Astra history one.", "Astra history two."):
                        observer.send(f"PRIVMSG {channel} :{text}")
                        expected_messages.extend(channel_messages(text, query_target))
                    wait_messages(proc, database_path, expected_messages)
                    wait_read_marker(proc, database_path, channel_id)
                    saved = save_state(proc, preferences, database_path)
                    layout = read_layout(preferences)
                    assert_workspace({"workspace": saved[0]}, channel_id, "hidden")
                    targets = composer_targets(window, layout, args.zoom)
                    state = observe_native_input(window, destination, "one-line-caret", "abcXYZ", 6, 6)
                    assert state["caret"][1] == targets["caret"][1], (
                        "The short standard draft unexpectedly wrapped", state, targets)
                    marker = GUI_DEBUG[window][1].stat().st_size
                    move_pointer(window, *targets["send"])
                    await_native_frame(window, marker, re.compile(r"wl_pointer[@#]\d+\.motion\("))
                    capture_window(window, destination, "one-line-send-history")
                    print("Focused native surface observation:", json.dumps({
                        "conversation": layout["conversation"], "caret": state["caret"], "send": targets["send"],
                        "window": window_geometry(window),
                        "ordinary_history": ["Astra history one.", "Astra history two."],
                    }), flush=True)
                    marker = GUI_DEBUG[window][1].stat().st_size
                    click(window, *targets["send"])
                    await_native_frame(window, marker,
                                       re.compile(r"wl_pointer[@#]\d+\.button\(\d+,\s*\d+,\s*272,\s*0\)"))
                    receive("PRIVMSG", [channel, "abcXYZ"], own_nick)
                    expected_messages.extend(channel_messages("abcXYZ", own_nick))
                    wait_messages(proc, database_path, expected_messages)
                    save_state(proc, preferences, database_path)
                    wait_rows(proc, database_path, "SELECT draft FROM buffers WHERE id=?", (channel_id,), [("",)])
                    stdout, stderr = finish_gui(proc, window, screenshot, destination, args.size)
                    logs.append(("focused-layout", stdout, stderr))
                    failed = False
                    print("Focused native surface/Send wire checks passed; inspect the capture for two complete history lines.", flush=True)
                    return
                if args.probes:
                    command("end-of-buffer", window=window)
                    observe_native_input(window, destination, "caret-focused-edit", "abcXYZ", 6, 6)
                command("beginning-of-buffer")
                key("ctrl+space")
                command("end-of-buffer", window=window if args.probes else None)
                if args.probes:
                    observe_native_input(window, destination, "native-selected-text", "abcXYZ", 6, 0)
                seed_clipboard("native-copy-sentinel", cleanup)
                key("alt+w")
                SESSION.until("native application selection clipboard", lambda: clipboard_text() == "abcXYZ")
                if args.probes:
                    framed_keys(window, "ctrl+g")
                    observe_native_input(window, destination, "caret-selection-cancelled", "abcXYZ", 6, 6)
                else:
                    key("ctrl+g")
                key("ctrl+a", "ctrl+d", "End", "ctrl+h", "Return")
                observed = receive("PRIVMSG", [channel, "bcXY"], endpoint["nick"])
                print("Graphical Emacs edit + IRC delivery:", observed)
                expected_messages.extend(channel_messages("bcXY", own_nick))
                key("ctrl+x", "b")
                type_text("missing-buffer")
                key("ctrl+g")
                type_text("after cancel")
                key("Return")
                receive("PRIVMSG", [channel, "after cancel"], endpoint["nick"])
                expected_messages.extend(channel_messages("after cancel", own_nick))
                if args.raw_only:
                    wait_messages(proc, database_path, expected_messages)
                    save_state(proc, preferences, database_path)
                    wait_rows(proc, database_path, "SELECT draft FROM buffers WHERE id=?", (channel_id,), [("",)])
                    capture_window(window, destination, "raw-no-ime-edit-send")
                    stdout, stderr = finish_gui(proc, window, screenshot, destination, args.size)
                    logs.append(("raw-no-ime", stdout, stderr))
                    failed = False
                    print("Raw NO-IME native keyboard/clipboard/selection/bcXY/cancel/exact IRC checks passed; no text-input metadata was required.", flush=True)
                    return
                draft = "draft survives"
                type_text(draft)
                saved = save_state(proc, preferences, database_path)
                assert_workspace({"workspace": saved[0]}, channel_id, "hidden")
                wait_rows(proc, database_path, "SELECT draft FROM buffers WHERE id=?",
                          (channel_id,), [(draft,)])
                key("Return")
                receive("PRIVMSG", [channel, draft], endpoint["nick"])
                expected_messages.extend(channel_messages(draft, own_nick))
                print("Native persistence preserves the active channel and durable draft: passed")
                private_incoming = "Private peer: incoming bubble."
                observer.send(f"PRIVMSG {own_nick} :{private_incoming}")
                expected_messages.append((query_id, query_target, private_incoming, 0, 0))
                wait_messages(proc, database_path, expected_messages)
                switch_buffer(query_target)
                selected = save_state(proc, preferences, database_path)
                assert_workspace({"workspace": selected[0]}, query_id, "hidden")
                assert selected[1] == (query_id,), selected
                private_own = "Private own: outgoing bubble."
                type_text(private_own)
                key("Return")
                receive("PRIVMSG", [query_target, private_own], own_nick)
                expected_messages.append((query_id, own_nick, private_own, 1, 0))
                layout = wait_layout(proc, preferences)
                assert layout["active_buffer"] == query_id, layout
                wait_messages(proc, database_path, expected_messages)
                capture_window(window, destination, "query-" + args.message_style + "-live")
                expected_messages.extend(probe_avy_cancel(
                    proc, window, preferences, database_path, args, destination,
                    lab=lab, cleanup=cleanup, own_nick=own_nick))
                expected_messages.extend(probe_native_formatting(
                    proc, window, preferences, database_path, args, destination,
                    query_id=query_id, query_target=query_target, own_nick=own_nick, receive=receive))
                if args.probes:
                    expected_messages.extend(probe_reply_paste(
                        proc, window, preferences, database_path, args, destination, observer,
                        query_id=query_id, query_target=query_target, own_nick=own_nick,
                        cleanup=cleanup, lab=lab, receive=receive))
                type_text("preview first line")
                key("shift+Return")
                type_text("preview second line")
                save_state(proc, preferences, database_path)
                wait_rows(proc, database_path, "SELECT draft FROM buffers WHERE id=?", (query_id,),
                          [("preview first line\npreview second line",)])
                capture_window(window, destination, "multiline-draft-preview")
                clear_draft()
                pending_text = "Pending real echo: no delivery claim."
                ergo = lab._children[0]  # LocalTopology starts its owned foreground Ergo first.
                assert Path(ergo.args[0]).name == "ergo", ergo.args
                os.kill(ergo.pid, signal.SIGSTOP)
                try:
                    type_text(pending_text)
                    key("Return")
                    wait_messages(proc, database_path, expected_messages +
                                  [(query_id, own_nick, pending_text, 1, 1)])
                    capture_window(window, destination, "pending-preview")
                finally:
                    os.kill(ergo.pid, signal.SIGCONT)
                receive("PRIVMSG", [query_target, pending_text], own_nick)
                expected_messages.append((query_id, own_nick, pending_text, 1, 0))
                formatted_preview = "\x02Preview bold\x02\tUTF-8 café ✨"
                observer.send(f"PRIVMSG {channel} :{formatted_preview}")
                expected_messages.extend(channel_messages(formatted_preview, query_target))
                wait_messages(proc, database_path, expected_messages)
                capture_window(window, destination, "formatted-utf8-preview")
                redacted_id = None
                if redaction:
                    deleted_preview = "Preview remove this latest message."
                    observer.send(f"PRIVMSG {channel} :{deleted_preview}")
                    wait_messages(proc, database_path, expected_messages +
                                  channel_messages(deleted_preview, query_target))
                    database = sqlite3.connect(f"file:{database_path}?mode=ro", uri=True)
                    try:
                        redacted_id, = database.execute(
                            "SELECT id FROM messages WHERE buffer=? AND text=?",
                            (channel_id, deleted_preview),
                        ).fetchone()
                    finally:
                        database.close()
                    assert redacted_id and not redacted_id.startswith("local:"), redacted_id
                    observer.send(f"REDACT {channel} {redacted_id}")
                    wait_rows(proc, database_path,
                              "SELECT buffer,text,deleted FROM messages WHERE id=? ORDER BY buffer",
                              (redacted_id,), [(channel_id, "", 1), (other_channel_id, "", 1)])
                    capture_window(window, destination, "deleted-latest-preview")
                else:
                    print("Real server lacks draft/message-redaction; deleted-preview visual case not exercised")
                other_incoming = "Other profile unread private message."
                observer.send(f"PRIVMSG {other_nick} :{other_incoming}")
                expected_messages.append((other_query_id, query_target, other_incoming, 0, 0))
                wait_messages(proc, database_path, expected_messages)
                select_server(proc, window, preferences, database_path, args, "Other")
                choose_avy_buffer(proc, window, preferences, database_path, args, other_query_id,
                                  "g", destination, "other-query")
                avy_route = "Native Avy routes the selected network private conversation."
                type_text(avy_route)
                key("Return")
                receive("PRIVMSG", [query_target, avy_route], other_nick)
                expected_messages.append((other_query_id, other_nick, avy_route, 1, 0))
                wait_messages(proc, database_path, expected_messages)
                select_server(proc, window, preferences, database_path, args, "Local")
                choose_avy_buffer(proc, window, preferences, database_path, args, query_id,
                                  "g", destination, "local-query")
                wait_read_marker(proc, database_path, query_id)
                layout = wait_layout(proc, preferences)
                original_navigation = layout["navigation_intent"]
                manual_navigation = original_navigation != "expanded"
                layout = expand_navigation(proc, window, preferences, database_path)
                filter_size = args.size
                if not sidebar_expanded(layout):
                    # Narrow shells retain avatar navigation; use a genuine
                    # temporary compositor configure for expanded filter controls.
                    filter_size = (max(args.size[0], math.ceil(1100 * args.zoom)),
                                   max(args.size[1], math.ceil(700 * args.zoom)))
                    SESSION.ipc("command", f"output HEADLESS-1 mode --custom {filter_size[0]}x{filter_size[1]}@60Hz")
                    SESSION.until("real wide sidebar configure", lambda:
                                  (window_geometry(window)["WIDTH"], window_geometry(window)["HEIGHT"]) == filter_size)
                    layout = expand_navigation(proc, window, preferences, database_path)
                assert sidebar_expanded(layout), layout
                baseline = save_filter_baseline(proc, preferences, database_path,
                                                query_id, "target-name/DMs")
                filter_sidebar(window, layout, args.zoom, query_target, 2)
                capture_window(window, destination, "filter-target-name")
                assert_same_state("Target-name/DM filter", baseline,
                                  save_state(proc, preferences, database_path))
                selector = filter_sidebar(window, layout, args.zoom, query_target, 2)
                activate_sidebar_item(window, 2, selector=selector)
                named_route = "Native target-name filter selects Local private conversation."
                type_text(named_route)
                key("Return")
                receive("PRIVMSG", [query_target, named_route], own_nick)
                expected_messages.append((query_id, own_nick, named_route, 1, 0))
                wait_messages(proc, database_path, expected_messages)
                wait_read_marker(proc, database_path, query_id)
                if manual_navigation:
                    layout = expand_navigation(proc, window, preferences, database_path)
                for choice, label, target in ((0, "all", channel), (1, "unread", channel),
                                               (2, "dms", query_target)):
                    switch_buffer(query_target, "Other")
                    wait_read_marker(proc, database_path, other_query_id)
                    switch_buffer(query_target)
                    select_server(proc, window, preferences, database_path, args, "Other")
                    layout = read_layout(preferences)
                    capture_window(window, destination, "server-Other-" + label)
                    if choice == 1:
                        unread_text = "Other channel becomes unread again."
                        observer.send(f"PRIVMSG {channel} :{unread_text}")
                        expected_messages.extend(channel_messages(unread_text, query_target))
                        wait_messages(proc, database_path, expected_messages)
                    baseline = save_filter_baseline(proc, preferences, database_path,
                                                    other_query_id, label + " selected network")
                    assert baseline[1] == (other_query_id,), baseline
                    if choice == 1:
                        assert next(row[1] for row in baseline[2] if row[0] == other_channel_id) > 0, baseline
                    filter_sidebar(window, layout, args.zoom, "Other", choice)
                    capture_window(window, destination, "filter-" + label)
                    after = save_state(proc, preferences, database_path)
                    assert_same_state(label + " network filter", baseline, after)
                    selector = filter_sidebar(window, layout, args.zoom, "Other", choice)
                    activate_sidebar_item(window, choice, selector=selector,
                                          activation="space" if choice == 1 else "Return",
                                          skip_pinned=choice == 1)
                    routed = f"Native {label} filter routes Other/{target}"
                    type_text(routed)
                    key("Return")
                    receive("PRIVMSG", [target, routed], other_nick)
                    if target == channel:
                        expected_messages.extend(channel_messages(routed, other_nick))
                    else:
                        expected_messages.append((other_query_id, other_nick, routed, 1, 0))
                    wait_messages(proc, database_path, expected_messages)
                    wait_read_marker(proc, database_path,
                                     other_channel_id if target == channel else other_query_id)
                    selected = save_state(proc, preferences, database_path)
                    expected_target = other_channel_id if target == channel else other_query_id
                    assert_workspace({"workspace": selected[0]}, expected_target, "hidden", server="Other")
                    assert selected[1] == (expected_target,), selected
                    select_server(proc, window, preferences, database_path, args, "Local")
                    switch_buffer(query_target)
                    wait_read_marker(proc, database_path, query_id)
                    if manual_navigation:
                        layout = expand_navigation(proc, window, preferences, database_path)
                switch_buffer(query_target)
                save_state(proc, preferences, database_path)
                layout = read_layout(preferences)
                capture_window(window, destination, "cross-server-conversation-restored")
                pinned_draft = "Pinned conversation keeps the private message destination."
                type_text(pinned_draft)
                baseline = save_filter_baseline(proc, preferences, database_path,
                                                query_id, "empty/clear")
                filter_sidebar(window, layout, args.zoom, "no-native-filter-match", 2)
                capture_window(window, destination, "filter-empty")
                assert_same_state("Empty filter", baseline,
                                  save_state(proc, preferences, database_path))
                command("end-of-buffer", window=window)
                pinned_state = _composer_native(window, pinned_draft, len(pinned_draft), len(pinned_draft))
                click(window, *composer_primary_targets(layout, args.zoom, pinned_state["caret"])["send"])
                receive("PRIVMSG", [query_target, pinned_draft], own_nick)
                expected_messages.append((query_id, own_nick, pinned_draft, 1, 0))
                wait_messages(proc, database_path, expected_messages)
                wait_read_marker(proc, database_path, query_id)
                capture_window(window, destination, "filter-pinned-send")
                baseline = save_filter_baseline(proc, preferences, database_path,
                                                query_id, "empty/clear after pinned send")
                selector = filter_sidebar(window, layout, args.zoom, "no-native-filter-match", 2)
                activate_sidebar_item(window, 2, selector=selector, skip_pinned=True)
                capture_window(window, destination, "filter-recovered")
                assert_same_state("Clear filters", baseline,
                                  save_state(proc, preferences, database_path))
                # Without changing either filter, keyboard activation must now
                # reach the restored channel row, not the no-match recovery action.
                controls = sidebar_controls(layout, args.zoom)
                click(window, *controls["query"])
                framed_keys(window, *(["Tab"] * (3 if selector else 6)), "Return")
                recovered = save_state(proc, preferences, database_path)
                assert recovered[1] == (channel_id,), recovered
                recovered_route = "Clear filters restores the actual channel navigation row."
                type_text(recovered_route)
                key("Return")
                receive("PRIVMSG", [channel, recovered_route], own_nick)
                expected_messages.extend(channel_messages(recovered_route, own_nick))
                wait_messages(proc, database_path, expected_messages)
                wait_read_marker(proc, database_path, channel_id)
                select_native_buffer(proc, window, preferences, database_path, args, query_id)
                wait_read_marker(proc, database_path, query_id)
                layout = read_layout(preferences)
                assert "settings" not in layout, layout
                filter_sidebar(window, layout, args.zoom, "no-native-filter-match", 2)
                move_pointer(window, *shell_actions(layout)["settings"])
                capture_window(window, destination, "settings-icon-hover")
                open_settings(window, preferences, from_icon=True)
                # Dialog metadata is live model state until a real user save;
                # observing disk is not a promise of save-on-open/autosave timing.
                framed_keys(window, "shift+Tab")
                settings_saved = save_settings_footer(window, saved_path=preferences)
                assert settings_saved["workspace"]["native_dialog"] == "settings", settings_saved["workspace"]
                settings_window = {"rect": tuple(float(value) for value in
                    settings_saved["workspace"]["native_dialog_rect"].split())}
                sx, sy = settings_window["rect"][:2]
                sw, sh = settings_window["rect"][2:]
                assert native_input_state(window) is None, "Actual Settings Save did not retain category navigation focus"
                assert 0 <= sx and 0 <= sy and sx + sw <= filter_size[0] and sy + sh <= filter_size[1], settings_window
                capture_window(window, destination, "settings-from-icon")
                marker = GUI_DEBUG[window][1].stat().st_size
                key("ctrl+g")
                await_native_frame(window, marker,
                                   re.compile(r"wl_keyboard[@#]\d+\.key\(\d+,\s*\d+,\s*\d+,\s*1\)"))
                save_state(proc, preferences, database_path)
                wait_layout(proc, preferences, predicate=lambda current: "settings" not in current)
                filter_sidebar(window, layout, args.zoom, "", 0)
                switch_buffer(query_target)
                if manual_navigation:
                    # Restore the original explicit sidebar intent after filtering.
                    set_navigation(proc, window, preferences, database_path,
                                   original_navigation, "filters-finished")
                if filter_size != args.size:
                    SESSION.ipc("command", f"output HEADLESS-1 mode --custom {args.size[0]}x{args.size[1]}@60Hz")
                    SESSION.until("restored real native smoke dimensions", lambda:
                                  (window_geometry(window)["WIDTH"], window_geometry(window)["HEIGHT"]) == args.size)
                    save_state(proc, preferences, database_path)
                print("Native combined filters preserve active conversation/unread and route duplicate targets: passed")
                private_draft = "private draft retained"
                type_text(private_draft)
                choose_avy_buffer(proc, window, preferences, database_path, args, channel_id,
                                  "b", destination, "retained-channel-draft")
                channel_peer = "Channel peer: dense left-aligned row."
                observer.send(f"PRIVMSG {channel} :{channel_peer}")
                expected_messages.extend(channel_messages(channel_peer, query_target))
                channel_own = "Channel own: also left aligned."
                type_text(channel_own)
                key("Return")
                receive("PRIVMSG", [channel, channel_own], own_nick)
                expected_messages.extend(channel_messages(channel_own, own_nick))
                channel_draft = "channel draft retained"
                type_text(channel_draft)
                choose_avy_buffer(proc, window, preferences, database_path, args, query_id,
                                  "g", destination, "retained-query-draft")
                wait_messages(proc, database_path, expected_messages)
                print("Exact real incoming/own channel and private-message destinations: passed")
                save_state(proc, preferences, database_path)
                database = sqlite3.connect(f"file:{database_path}?mode=ro", uri=True)
                try:
                    history_query = (
                        "SELECT buffer,id,nick,text,own,pending,deleted FROM messages "
                        "WHERE buffer IN (?,?) ORDER BY buffer,sort_time,seq")
                    history_before = database.execute(history_query, (channel_id, query_id)).fetchall()
                    for identity in (channel_id, query_id):
                        select_native_buffer(proc, window, preferences, database_path, args, identity)
                        wait_read_marker(proc, database_path, identity)
                        selected = save_state(proc, preferences, database_path)
                        assert_workspace({"workspace": selected[0]}, identity, "hidden")
                        assert selected[1] == (identity,), selected
                        assert_saved_buffers(database_path, [
                            (channel_id, "Local", channel, "channel", channel_draft),
                            (query_id, "Local", query_target, "query", private_draft),
                        ], identity)
                        history_after = database.execute(history_query, (channel_id, query_id)).fetchall()
                        assert history_after == history_before, (history_before, history_after)
                finally:
                    database.close()
                print("Global buffer selection preserves both distinct drafts and exact histories: passed")
                probe_three_way_panels(proc, window, preferences, database_path, args, destination, channel_id)
                default_navigation(proc, window, query_id, "initial")
                first_capture = destination.with_name(destination.stem + "-navigation-default.bmp")
                stdout, stderr = finish_gui(proc, window, screenshot, first_capture, args.size)
                logs.append(("navigation-default", stdout, stderr))
                settings = tomllib.loads(preferences.read_text())
                assert math.isclose(settings["appearance"]["scale"], args.zoom, abs_tol=1e-6), settings
                assert settings["appearance"]["theme_source"] == "auto", settings
                assert settings["appearance"]["reduced_motion"] is args.reduced_motion, settings
                assert_workspace(settings, query_id, "hidden")
                assert settings["workspace"]["native_navigation_mode"] == "expanded", settings
                original_channel_draft = channel_draft
                previous_active, previous_people = query_id, "hidden"
                previous_navigation = "expanded"
                presentation_expected = settings["presentation"]["irc_channels"]
                restart_evidence = []
                for phase in ("people-visible", "query-suppressed", "channel-return", "people-compact",
                              "compact-query-suppressed", "compact-channel-return", "people-hidden",
                              "message-irc", "message-bubbles", "final"):
                    screenshot = tmp / (phase + ".bmp")
                    stdout = stderr = None
                    proc, window = launch_gui(binary, screenshot, env, args.size, logs)
                    wait_joins(observer, channel, (own_nick, other_nick))
                    restored = tomllib.loads(preferences.read_text())
                    assert_workspace(restored, previous_active, previous_people)
                    assert restored["workspace"]["native_navigation_mode"] == previous_navigation, restored
                    assert restored["presentation"]["irc_channels"] == presentation_expected, restored
                    assert_saved_buffers(database_path, [
                        (channel_id, "Local", channel, "channel", channel_draft),
                        (query_id, "Local", query_target, "query", private_draft),
                    ], previous_active)
                    live = save_state(proc, preferences, database_path)
                    assert_workspace({"workspace": live[0]}, previous_active, previous_people)
                    assert live[0]["native_navigation_mode"] == previous_navigation, live
                    restart_evidence.append({"phase": phase, "stage": "restored",
                                             "workspace": live[0], "buffers": live[2],
                                             "capture": str(capture_window(window, destination, "restored-" + phase))})
                    if phase != "people-hidden":
                        resize_panel_viewport(window, (max(args.size[0], math.ceil(1300 * args.zoom)),
                                                       max(args.size[1], math.ceil(700 * args.zoom))))
                    if phase in ("message-bubbles", "final"):
                        mode = "irc" if phase == "message-bubbles" else "bubbles"
                        save_state(proc, preferences, database_path)
                        capture_window(window, destination, "channel-" + mode + "-after-restart")
                    if phase == "people-visible":
                        select_native_buffer(proc, window, preferences, database_path, args, channel_id)
                        type_text(" [restored]")
                        channel_draft += " [restored]"
                        set_people(proc, window, preferences, database_path, "expanded")
                        active_buffer, people = channel_id, "expanded"
                    elif phase == "query-suppressed":
                        key("Return")
                        receive("PRIVMSG", [channel, channel_draft], own_nick)
                        expected_messages.extend(channel_messages(channel_draft, own_nick))
                        channel_draft = original_channel_draft
                        type_text(channel_draft)
                        select_native_buffer(proc, window, preferences, database_path, args, query_id)
                        type_text(" [restored]")
                        key("Return")
                        private_restored = private_draft + " [restored]"
                        receive("PRIVMSG", [query_target, private_restored], own_nick)
                        expected_messages.append((query_id, own_nick, private_restored, 1, 0))
                        type_text(private_draft)
                        set_navigation(proc, window, preferences, database_path,
                                       "compact", "query-independent-left-compact")
                        active_buffer, people = query_id, "expanded"
                    elif phase == "channel-return":
                        select_native_buffer(proc, window, preferences, database_path, args, channel_id)
                        capture_window(window, destination, "channel-return")
                        members = save_state(proc, preferences, database_path)[0]["people_mode"]
                        set_navigation(proc, window, preferences, database_path, "compact", "members-sidebar-compact")
                        assert save_state(proc, preferences, database_path)[0]["people_mode"] == members
                        capture_window(window, destination, "members-independent-sidebar-compact")
                        set_navigation(proc, window, preferences, database_path, "expanded", "members-sidebar-expanded")
                        capture_window(window, destination, "members-independent-sidebar-expanded")
                        set_navigation(proc, window, preferences, database_path,
                                       "hidden", "members-independent-sidebar-hidden")
                        active_buffer, people = channel_id, "expanded"
                    elif phase == "people-compact":
                        set_people(proc, window, preferences, database_path, "compact")
                        active_buffer, people = channel_id, "compact"
                    elif phase == "compact-query-suppressed":
                        select_native_buffer(proc, window, preferences, database_path, args, query_id)
                        active_buffer, people = query_id, "compact"
                    elif phase == "compact-channel-return":
                        select_native_buffer(proc, window, preferences, database_path, args, channel_id)
                        active_buffer, people = channel_id, "compact"
                    elif phase == "people-hidden":
                        native_layout = read_layout(preferences)
                        actions = title_actions(native_layout, args.zoom)
                        point = actions["people"]
                        print("People native action target:", json.dumps({
                            "phase": phase, "zoom": args.zoom,
                            "conversation": native_layout["conversation"], "point": point,
                            "window": window_geometry(window),
                        }), flush=True)
                        move_pointer(window, *point)
                        capture_window(window, destination, "people-icon-hover")
                        click(window, *point)
                        if save_state(proc, preferences, database_path)[0]["people_mode"] != "hidden":
                            # Only the constrained header moves this action into More.
                            # On a full header this position opens Presence instead,
                            # so a broken direct toggle cannot be hidden by the fallback.
                            popup = open_title_menu(window, actions)
                            capture_window(window, destination, "people-overflow-menu")
                            select_native_menu(window, popup, (2,))
                        active_buffer, people = channel_id, "hidden"
                    elif phase in ("message-irc", "message-bubbles"):
                        mode = phase.removeprefix("message-")
                        presentation_expected = set_message_style(
                            proc, window, preferences, database_path, args, destination,
                            channel_id, mode, label="channel-" + mode + "-before-restart")
                        assert (query_id in presentation_expected) == (args.message_style == "irc"), presentation_expected
                        text = f"Channel {mode}: genuine refreshed display row."
                        observer.send(f"PRIVMSG {channel} :{text}")
                        expected_messages.extend(channel_messages(text, query_target))
                        wait_messages(proc, database_path, expected_messages)
                        wait_read_marker(proc, database_path, channel_id)
                        capture_window(window, destination, "channel-" + mode + "-live")
                        active_buffer, people = channel_id, "hidden"
                    else:
                        live = save_state(proc, preferences, database_path)
                        assert_workspace({"workspace": live[0]}, channel_id, "hidden")
                        compact_layout = set_navigation(proc, window, preferences, database_path, "compact", "explicit-compact")
                        assert compact_layout["navigation_intent"] == "compact", compact_layout
                        capture_window(window, destination, "navigation-avatar-compact")
                        set_navigation(proc, window, preferences, database_path, "expanded", "explicit-expanded")
                        capture_window(window, destination, "navigation-expanded-intent")
                        default_layout, default_navigation_expanded = default_navigation(
                            proc, window, channel_id, "final")
                        capture_window(window, destination, "navigation-default")
                        select_native_buffer(proc, window, preferences, database_path, args, query_id)
                        clear_draft()
                        final_incoming = "Private peer: final incoming."
                        observer.send(f"PRIVMSG {own_nick} :{final_incoming}")
                        expected_messages.append((query_id, query_target, final_incoming, 0, 0))
                        final_own = "Private own: final outgoing."
                        type_text(final_own)
                        key("Return")
                        receive("PRIVMSG", [query_target, final_own], own_nick)
                        expected_messages.append((query_id, own_nick, final_own, 1, 0))
                        type_text(private_draft)
                        active_buffer, people = query_id, "hidden"
                    wait_messages(proc, database_path, expected_messages)
                    if phase == "final":
                        final_layout = wait_layout(
                            proc, preferences,
                            predicate=lambda layout: layout["navigation_intent"] == "expanded" and
                            layout["active_buffer"] == query_id)
                        log_navigation("final-query-default", default_layout, final_layout, window)
                    resize_panel_viewport(window, args.size)
                    phase_state = save_state(proc, preferences, database_path)
                    assert_workspace({"workspace": phase_state[0]}, active_buffer, people)
                    restart_evidence.append({"phase": phase, "stage": "before-restart",
                                             "workspace": phase_state[0], "buffers": phase_state[2],
                                             "capture": str(capture_window(window, destination, "panels-restart-" + phase))})
                    target = destination if phase == "final" else destination.with_name(
                        destination.stem + "-" + phase + ".bmp")
                    stdout, stderr = finish_gui(proc, window, screenshot, target, args.size)
                    logs.append((phase, stdout, stderr))
                    settings = tomllib.loads(preferences.read_text())
                    assert_workspace(settings, active_buffer, people)
                    assert_saved_buffers(database_path, [
                        (channel_id, "Local", channel, "channel", channel_draft),
                        (query_id, "Local", query_target, "query", private_draft),
                    ], active_buffer)
                    previous_navigation = settings["workspace"]["native_navigation_mode"]
                    previous_active, previous_people = active_buffer, people
                destination.with_name(destination.stem + "-panels-restarts.json").write_text(
                    json.dumps(restart_evidence, indent=2, ensure_ascii=False) + "\n")
                for phase, suffix in (("disconnected", " [offline attempt]"),
                                      ("recovered-restart", " [after restart]")):
                    screenshot = tmp / (phase + ".bmp")
                    stdout = stderr = None
                    proc, window = launch_gui(binary, screenshot, env, args.size, logs)
                    wait_joins(observer, channel, (own_nick, other_nick))
                    assert_workspace(tomllib.loads(preferences.read_text()), query_id, "hidden")
                    assert_saved_buffers(database_path, [
                        (channel_id, "Local", channel, "channel", channel_draft),
                        (query_id, "Local", query_target, "query", private_draft),
                    ], query_id)
                    disconnect_local(proc)
                    attempted = private_draft + suffix
                    type_text(suffix)
                    native_layout = read_layout(preferences)
                    icons = composer_targets(window, native_layout, args.zoom)
                    sx, sy = icons["send"]
                    print("Disconnected native Send hit target:", json.dumps({
                        "phase": phase, "point": [sx, sy], "conversation": native_layout["conversation"],
                        "client": window_geometry(window),
                    }), flush=True)
                    move_pointer(window, sx, sy)
                    pointer_button(True)
                    try:
                        capture_window(window, destination, phase + "-send-held")
                    finally:
                        pointer_button(False)
                    continuation = " [still editing]"
                    type_text(continuation)
                    save_state(proc, preferences, database_path)
                    database = sqlite3.connect(f"file:{database_path}?mode=ro", uri=True)
                    try:
                        recovered = database.execute("SELECT draft FROM buffers WHERE id=?",
                                                     (query_id,)).fetchone()
                    finally:
                        database.close()
                    accepted = (attempted + continuation, attempted + "\n" + continuation)
                    assert recovered in [(text,) for text in accepted], {
                        "attempted": attempted, "continuation": continuation,
                        "accepted": accepted, "actual": recovered,
                    }
                    retained = recovered[0]
                    assert_saved_buffers(database_path, [
                        (channel_id, "Local", channel, "channel", channel_draft),
                        (query_id, "Local", query_target, "query", retained),
                    ], query_id)
                    wait_rows(proc, database_path,
                              "SELECT buffer,text,own,pending FROM messages WHERE text IN (?,?)",
                              (attempted, retained), [])
                    capture_window(window, destination, phase + "-draft-recovery")
                    private_draft = retained
                    target = destination.with_name(destination.stem + "-" + phase + ".bmp")
                    stdout, stderr = finish_gui(proc, window, screenshot, target, args.size)
                    logs.append((phase, stdout, stderr))
                    assert_workspace(tomllib.loads(preferences.read_text()), query_id, "hidden")
                    assert_saved_buffers(database_path, [
                        (channel_id, "Local", channel, "channel", channel_draft),
                        (query_id, "Local", query_target, "query", private_draft),
                    ], query_id)
                    restored_layout = read_layout(preferences)
                    assert restored_layout["active_buffer"] == query_id, restored_layout
                print("Real disconnected Send preserves editable shared draft across quit/restart: passed")
                if redacted_id is not None:
                    database = sqlite3.connect(f"file:{database_path}?mode=ro", uri=True)
                    try:
                        deleted = database.execute(
                            "SELECT buffer,text,deleted FROM messages WHERE id=? ORDER BY buffer",
                            (redacted_id,),
                        ).fetchall()
                        assert deleted == [(channel_id, "", 1), (other_channel_id, "", 1)], deleted
                    finally:
                        database.close()
                print("Single visible chat, server rail, independent channels/members, display-mode and draft restart persistence: passed")
                if args.probes:
                    screenshot = tmp / "native-refinements.bmp"
                    stdout = stderr = None
                    proc, window = launch_gui(binary, screenshot, env, args.size, logs)
                    wait_joins(observer, channel, (own_nick, other_nick))
                    save_state(proc, preferences, database_path)
                    assert_workspace(tomllib.loads(preferences.read_text()), query_id, "hidden")
                    database = sqlite3.connect(f"file:{database_path}?mode=ro", uri=True)
                    try:
                        history_query = (
                            "SELECT buffer,id,nick,text,own,pending,deleted FROM messages "
                            "WHERE buffer IN (?,?) AND presence=0 ORDER BY buffer,sort_time,seq")
                        history_before = database.execute(history_query, (channel_id, query_id)).fetchall()
                        settings_rows = probe_settings_categories(
                            proc, window, preferences, config / "config.toml", database_path, args, destination,
                            observer=observer, query_target=query_target, query_id=query_id, own_nick=own_nick,
                            cleanup=cleanup, lab=lab, receive=receive)
                        expected_messages.extend((buffer, nick, text, own, pending)
                                                 for buffer, identity, nick, text, own, pending, deleted in settings_rows)
                        # Only the two independently wire-verified Save-failure/
                        # still-connected rows are permitted additions. Preserve
                        # the full original history before native refresh operations.
                        history_after_settings = database.execute(history_query, (channel_id, query_id)).fetchall()
                        settings_keys = {(row[0], row[1]) for row in settings_rows}
                        assert [row for row in history_after_settings if (row[0], row[1]) not in settings_keys] == history_before
                        assert sorted(row for row in history_after_settings if (row[0], row[1]) in settings_keys) == sorted(settings_rows)
                        history_before = history_after_settings
                        probe_flat_refresh(
                            proc, window, preferences, database_path, args, destination,
                            observer=observer, query_target=query_target, query_id=query_id,
                            other_query_id=other_query_id, cleanup=cleanup, lab=lab)
                        assert database.execute(history_query, (channel_id, query_id)).fetchall() == history_before, "Native refinement controls changed actual conversation histories"
                        assert_saved_buffers(database_path, [
                            (channel_id, "Local", channel, "channel", channel_draft),
                            (query_id, "Local", query_target, "query", private_draft),
                        ], query_id)
                    finally:
                        database.close()
                    target = destination.with_name(destination.stem + "-native-refinements.bmp")
                    stdout, stderr = finish_gui(proc, window, screenshot, target, args.size)
                    logs.append(("native-refinements", stdout, stderr))
                    assert_workspace(tomllib.loads(preferences.read_text()), query_id, "hidden")
                failed = False
            finally:
                if failed and proc is not None and window and proc.poll() is None:
                    try:
                        capture_window(window, destination, "failed-native")
                    except (OSError, subprocess.SubprocessError) as error:
                        print(f"Could not capture failing native window: {error}", file=sys.stderr)
                if proc is not None:
                    if proc.poll() is None:
                        proc.terminate()
                    if stdout is None or stderr is None:
                        try:
                            stdout, stderr = collect_gui(proc, timeout=10)
                        except subprocess.TimeoutExpired:
                            proc.kill()
                            stdout, stderr = collect_gui(proc)
                if not failed:
                    destination.with_suffix(".wayland.log").write_text(lab._redact("\n".join(
                        f"=== GUI {label} ===\n{error}" for label, _, error in logs)))
                    destination.with_suffix(".settings.toml").write_text(lab._redact(preferences.read_text()))
                if failed:
                    print(f"GUI window={window} Wayland={env['WAYLAND_DISPLAY']} "
                          f"zoom={args.zoom:g} size={args.size[0]}x{args.size[1]}", file=sys.stderr)
                    if screenshot.exists():
                        failure_capture = Path(str(destination) + ".failed.bmp")
                        try:
                            failure_capture.write_bytes(screenshot.read_bytes())
                            print("Failure screenshot:", failure_capture, file=sys.stderr)
                        except OSError as error:
                            print(f"Could not preserve failure screenshot: {error}", file=sys.stderr)
                    if preferences.exists():
                        try:
                            layout_capture = destination.with_name(destination.stem + ".failed-settings.toml")
                            layout_capture.write_text(lab._redact(preferences.read_text()))
                            print("Failure native layout:", layout_capture, file=sys.stderr)
                        except OSError as error:
                            print(f"Could not preserve failure native layout: {error}", file=sys.stderr)
                    if proc is not None:
                        logs.append((f"current (exit {proc.returncode})", stdout or "", stderr or ""))
                    report = "\n".join(
                        f"=== GUI {label} ===\nstdout:\n{out}\nstderr:\n{error}"
                        for label, out, error in logs)
                    report += "\n" + "\n".join(
                        f"=== fixture {name} ===\n{path.read_text(errors='replace')}"
                        for name, path in lab.logs.items())
                    report += "\n=== compositor ===\n" + (tmp / "compositor/sway.log").read_text(errors="replace")
                    report = lab._redact(report)
                    print(report, file=sys.stderr)
                    try:
                        log_path = destination.with_name(destination.stem + ".failed.log")
                        log_path.write_text(report)
                        print("Failure log:", log_path, file=sys.stderr)
                    except OSError as error:
                        print(f"Could not preserve failure log: {error}", file=sys.stderr)
                    lab.diagnostics()


if __name__ == "__main__":
    run()
