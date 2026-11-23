#!/usr/bin/env python3
"""Capture README images from BlastIRC with a private Wayland and IRC fixture."""
import argparse
from contextlib import ExitStack, closing
from datetime import datetime, timedelta, timezone
import json
import os
from pathlib import Path
import re
import socketserver
import sqlite3
import subprocess
import tempfile
from threading import Thread

from dpi_smoke import HeadlessSway, capture_png, stop_group, tree_windows
import gui_smoke as gui

ROOT = Path(__file__).resolve().parents[1]
SIZE = (1440, 900)
DPR = 1.25
NETWORK = "Moonbase"
NICK = "sol"
CHANNEL = "#observatory"
QUERY = "nova"
CHATS = (CHANNEL, "#mission-control", "#hardware", "#off-topic", QUERY, "mira", "orbitbot")


class DemoIRC(socketserver.StreamRequestHandler):
    def handle(self):
        def send(line):
            self.wfile.write((line + "\r\n").encode())

        for raw in self.rfile:
            command, _, args = raw.decode().strip().partition(" ")
            if command == "CAP" and args.startswith("LS"):
                send(":irc.demo CAP * LS :message-tags server-time")
            elif command == "CAP" and args.startswith("REQ"):
                send(f":irc.demo CAP {NICK} ACK :{args[4:].lstrip(':')}")
            elif command == "CAP" and args == "END":
                send(f":irc.demo 001 {NICK} :Welcome to Moonbase")
                send(f":irc.demo 005 {NICK} CHANTYPES=# PREFIX=(ov)@+ CASEMAPPING=rfc1459 :supported")
                send(f":irc.demo 376 {NICK} :End of MOTD")
            elif command == "JOIN":
                for channel in args.split(","):
                    send(f":{NICK}!demo@localhost JOIN :{channel}")
                    send(f":irc.demo 332 {NICK} {channel} :Night sky sketches and telescope projects")
                    send(f":irc.demo 353 {NICK} = {channel} :@nova +mira {NICK} orbitbot")
                    send(f":irc.demo 366 {NICK} {channel} :End of names")
            elif command == "PING":
                send(f":irc.demo PONG irc.demo :{args.lstrip(':')}")


def seed(database):
    # The app creates and migrates its own database before synthetic rows are added.
    with closing(sqlite3.connect(database)) as db, db:
        db.create_function("server_nanoseconds", 1, lambda _: 0, deterministic=True)
        for target in CHATS:
            kind = "channel" if target.startswith("#") else "query"
            db.execute("INSERT OR REPLACE INTO buffers(id,server,target,title,kind) VALUES (?,?,?,?,?)",
                       (f"{NETWORK}/{target}", NETWORK, target, target, kind))
        db.execute("INSERT OR REPLACE INTO meta VALUES ('active_buffer',?)", (f"{NETWORK}/{CHANNEL}",))
        base = datetime(2026, 10, 8, 21, 14, tzinfo=timezone.utc)
        conversations = {
            CHANNEL: (
                ("map", "nova", 2, "First light! The new mount tracked Saturn for twenty minutes.", "", ""),
                ("rings", "nova", 15, "I could pick out the rings before the clouds came in.", "", ""),
                ("test", "mira", 35, "I’ll bring the wide-angle lens next time.", "", ""),
                ("calibrate", NICK, 62, "The calibration notes are ready for the next run.", "", ""),
                ("sky", "orbitbot", 78, "Forecast: clear skies after 22:00 local time.", "", ""),
                ("mention", "nova", 122, "sol: want to run the rooftop session on Friday?", "", "🔭"),
                ("confirm", NICK, 145, "Count me in. I’ll bring a thermos.", "", ""),
            ),
            QUERY: (
                ("poster", "nova", 3, "I finished the poster for Friday’s stargazing session.", "", "✨"),
                ("palette", "nova", 26, "Midnight blue with a little silver for the star trails.", "", ""),
                ("reply", NICK, 65, "That sounds perfect. Can you send a print-ready copy?", "poster", ""),
                ("print", "nova", 124, "Absolutely. I’ll bring a few extras for the sign-in table.", "", ""),
            ),
            "#mission-control": (
                ("schedule", "mira", 150, "The rooftop is reserved for Friday.", "", ""),
                ("weather", "orbitbot", 164, "Wind should stay below ten knots.", "", ""),
                ("station", "nova", 169, "I’ll set up by the west railing.", "", ""),
                ("checklist", "mira", 195, "The battery checklist is ready.", "", ""),
            ),
            "#hardware": (
                ("washer", "nova", 173, "The new washer fits the tripod plate.", "", ""),
                ("wrench", "mira", 199, "I left the wrench in the blue case.", "", ""),
            ),
            "mira": (("lens", "mira", 200, "The wide-angle lens is packed.", "", ""),),
        }
        for target, rows in conversations.items():
            buffer = f"{NETWORK}/{target}"
            for key, author, seconds, text, reply, reaction in rows:
                instant = base + timedelta(seconds=seconds)
                stamp = int(instant.timestamp())
                seq = db.execute(
                    "INSERT INTO messages(buffer,id,nick,text,time,reply,label,timestamp,received_at,"
                    "sort_time,own,pending,deleted,mention,action,presence) VALUES (?,?,?,?,?,?,?,?,?,?,?,0,0,?,0,0)",
                    (buffer, key, author, text, instant.strftime("%Y-%m-%dT%H:%M:%SZ"), reply, "", stamp,
                     stamp, stamp, author == NICK, key == "mention"),
                ).lastrowid
                if reaction:
                    db.executemany("INSERT INTO reactions(message,emoji,nick,ordinal) VALUES (?,?,?,?)",
                                   [(seq, reaction, nick, order) for order, nick in enumerate((NICK, "mira"))])
            db.execute("INSERT INTO history_state VALUES (?,?,'',?,'',1)", (buffer, rows[0][0], rows[-1][0]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build-native/blastirc")
    parser.add_argument("--output", type=Path, default=ROOT / "assets")
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    output = args.output.resolve()
    with ExitStack() as cleanup:
        tmp = Path(cleanup.enter_context(tempfile.TemporaryDirectory(prefix="blastirc-readme-")))
        env = dict(os.environ, LIBGL_ALWAYS_SOFTWARE="1", SDL_AUDIODRIVER="dummy", TZ="UTC", LC_ALL="C.UTF-8")
        for key, directory in (("HOME", "home"), ("XDG_CONFIG_HOME", "config"), ("XDG_DATA_HOME", "data"),
                               ("XDG_STATE_HOME", "state"), ("XDG_CACHE_HOME", "cache")):
            (tmp / directory).mkdir(mode=0o700)
            env[key] = str(tmp / directory)
        config = tmp / "config/blastirc"
        config.mkdir(mode=0o700)
        server = cleanup.enter_context(socketserver.ThreadingTCPServer(("127.0.0.1", 0), DemoIRC))
        server.daemon_threads = True
        thread = Thread(target=server.serve_forever, daemon=True)
        thread.start()
        cleanup.callback(server.shutdown)
        profile = config / "config.toml"
        profile.write_text(f'''schema_version = 1
[[identity]]
name = "demo"
nick = "{NICK}"
username = "demo"
realname = "Synthetic screenshot profile"
[[server]]
name = "{NETWORK}"
address = "127.0.0.1"
port = {server.server_address[1]}
tls = false
identity = "demo"
channels = {json.dumps([target for target in CHATS if target.startswith('#')])}
''')
        profile.chmod(0o600)
        settings = config / "settings.toml"
        preview_settings = f'''friends = ["nova"]
[appearance]
scale = 1.0
message_scale = 1.05
avatar_style = "sprite"
reduced_motion = true
theme_source = "builtin"
[history]
retention_days = 0
[presentation]
channel_message_layout = "stacked"
direct_message_layout = "bubbles"
pinned_buffers = ["{NETWORK}/{CHANNEL}"]
[workspace]
native_navigation_mode = "expanded"
native_server_rail_visible = "true"
people_mode = "compact"
'''
        settings.write_text(preview_settings)
        settings.chmod(0o600)
        sway = cleanup.enter_context(HeadlessSway(tmp / "runtime", tmp / "compositor", env,
                                     outputs=(("HEADLESS-1", *SIZE, 0, 0, DPR),)))
        gui.SESSION = sway
        seat = cleanup.enter_context(gui.VirtualSeat(Path(sway.env["XDG_RUNTIME_DIR"]) / sway.env["WAYLAND_DISPLAY"]))
        subprocess.run([binary, "--frames", "1"], env=sway.env, check=True, capture_output=True, timeout=15)
        settings.write_text(preview_settings)
        seed(tmp / "data/blastirc/state.sqlite3")
        log = cleanup.enter_context((tmp / "client.log").open("wb"))
        client = subprocess.Popen([binary], env=sway.env, stdout=log, stderr=log, start_new_session=True)
        cleanup.callback(stop_group, client)
        sway.until("demo window", lambda: any(node.get("pid") == client.pid for node, _ in tree_windows(sway.ipc("get_tree"))))
        logical = tuple(round(dimension / DPR) for dimension in SIZE)
        seat.move(logical[0] // 2, 1, *logical)

        def capture(name, *expected_text):
            screenshot = tmp / name
            words = ""

            def ready():
                nonlocal words
                capture_png(sway.env, screenshot, scale=DPR, expected_pixels=SIZE, output="HEADLESS-1")
                text = subprocess.check_output(["tesseract", str(screenshot), "stdout", "--psm", "11"],
                                               stderr=subprocess.DEVNULL, text=True, timeout=8)
                words = re.sub(r"\s+", " ", text).casefold()
                return all(re.search(r"(?<!\w)" + re.escape(probe.casefold()) + r"(?!\w)", words)
                           for probe in expected_text)

            try:
                sway.until(name + " fixture text", ready)
            except RuntimeError as error:
                raise RuntimeError(f"{error}; rendered text: {words}") from error

        capture("screenshot.png", "First light", "rooftop session", "Connected")
        gui.key("ctrl+x", "b")
        capture("screenshot-chats.png", "Chats", "mission-control", "observatory")
        gui.type_text(QUERY)
        gui.key("Return")
        capture("screenshot-dm.png", "poster", "midnight blue", "sign-in")
        output.mkdir(parents=True, exist_ok=True)
        for name in ("screenshot.png", "screenshot-chats.png", "screenshot-dm.png"):
            (output / name).write_bytes((tmp / name).read_bytes())
            print(output / name)


if __name__ == "__main__":
    main()
