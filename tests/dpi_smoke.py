#!/usr/bin/env python3
"""Exercise one real native Noctalia window on a private two-output Sway session.

Run inside the project's nix develop shell, for example:
  python3 tests/dpi_smoke.py \
    --output-dir /tmp/blastirc-dpi \
    -- build-native/blastirc --frames 1800

The client must remain alive for the scenario. Screenshots, native Wayland
protocol logs, and report.json are retained; the private runtime is removed.
This checks native configure/ack/commit responsiveness and pixel dimensions,
not font metrics. No host display, IPC socket, config, or environment is changed.
"""

import argparse
from contextlib import ExitStack
import json
import math
import os
from pathlib import Path
import re
import shutil
import signal
import stat
import struct
import subprocess
import sys
import tempfile
import time

LOGICAL = (1440, 960)
PROBE = (1280, 800)
STAGES = (("01-1x", "HEADLESS-1", 1.0),
          ("02-1.25x", "HEADLESS-2", 1.25),
          ("03-1.5x", "HEADLESS-2", 1.5),
          ("04-2x", "HEADLESS-2", 2.0),
          ("05-back-1x", "HEADLESS-1", 1.0))


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def tree_windows(node, output=None):
    if node.get("type") == "output":
        output = node["name"]
    if node.get("pid") is not None:
        yield node, output
    for child in node.get("nodes", []) + node.get("floating_nodes", []):
        yield from tree_windows(child, output)


def main_surface_ids(path):
    text = Path(path).read_text(errors="replace")
    surfaces = re.findall(r"xdg_wm_base[@#]\d+\.get_xdg_surface\(new id xdg_surface[@#](\d+), wl_surface[@#](\d+)\)", text)
    tops = re.findall(r"xdg_surface[@#](\d+)\.get_toplevel\(new id xdg_toplevel[@#](\d+)\)", text)
    require(len(tops) == 1,
            "could not uniquely associate native xdg-shell surface in client.stderr.log")
    surfaces = [surface for surface in surfaces if surface[0] == tops[0][0]]
    require(len(surfaces) == 1,
            "could not uniquely associate native xdg-shell surface in client.stderr.log")
    return int(surfaces[0][1]), int(surfaces[0][0]), int(tops[0][1])


def capture_png(env, path, *, scale, expected_pixels, region=None, output=None, timeout=30):
    path = Path(path)
    argv = ["grim"]
    if region is not None:
        argv.extend(("-g", region))
    elif output is not None:
        argv.extend(("-o", output))
    argv.extend(("-s", str(scale), str(path)))
    capture = subprocess.run(argv, env=env, capture_output=True, text=True, timeout=timeout)
    require(capture.returncode == 0,
            f"{path.stem}: grim failed ({capture.returncode}): {capture.stderr.strip()}")
    with path.open("rb") as stream:
        header = stream.read(24)
    require(header[:8] == b"\x89PNG\r\n\x1a\n" and header[12:16] == b"IHDR",
            f"{path.stem}: grim did not produce a PNG")
    dimensions = struct.unpack(">II", header[16:24])
    require(dimensions == expected_pixels,
            f"{path.stem}: screenshot is {dimensions}, expected native physical pixels {expected_pixels}")
    return dimensions


def protocol_evidence(path, ids, offset, logical, scale):
    """Match a configure/ack and its same-surface, correctly sized buffer commit."""
    surface, xdg, top = ids
    expected = tuple(round(size * scale) for size in logical)
    buffers, params = {}, {}
    attached = None
    wanted = acked = False
    serial = None
    evidence = None
    position = 0
    create_shm = re.compile(r"wl_shm_pool[@#]\d+\.create_buffer\(new id wl_buffer[@#](\d+), \d+, (\d+), (\d+),")
    create_drm = re.compile(r"wl_drm[@#]\d+\.create_(?:prime_|planar_)?buffer\(new id wl_buffer[@#](\d+), [^,]+, (\d+), (\d+),")
    create_immed = re.compile(r"zwp_linux_buffer_params_v1[@#]\d+\.create_immed\(new id wl_buffer[@#](\d+), (\d+), (\d+),")
    create_async = re.compile(r"zwp_linux_buffer_params_v1[@#](\d+)\.create\((\d+), (\d+),")
    created_async = re.compile(r"zwp_linux_buffer_params_v1[@#](\d+)\.created\(new id wl_buffer[@#](\d+)\)")
    attach = re.compile(rf"wl_surface[@#]{surface}\.attach\((?:wl_buffer[@#](\d+)|nil),")
    commit = re.compile(rf"wl_surface[@#]{surface}\.commit\(")
    configured = re.compile(rf"xdg_toplevel[@#]{top}\.configure\((\d+), (\d+),")
    serial_event = re.compile(rf"xdg_surface[@#]{xdg}\.configure\((\d+)\)")
    ack_event = re.compile(rf"xdg_surface[@#]{xdg}\.ack_configure\((\d+)\)")
    # EGL may negotiate SHM or dma-buf. Dimensions must come from the actual
    # buffer protocol, never from output scale or a screenshot-size inference.
    for raw in path.read_bytes().splitlines(keepends=True):
        recent = position >= offset
        position += len(raw)
        line = raw.decode("utf-8", errors="replace")
        for pattern, transport in ((create_shm, "shm"), (create_drm, "wl_drm"),
                                   (create_immed, "linux-dmabuf")):
            match = pattern.search(line)
            if match:
                buffers[int(match[1])] = ((int(match[2]), int(match[3])), transport)
        match = create_async.search(line)
        if match:
            params[int(match[1])] = ((int(match[2]), int(match[3])), "linux-dmabuf")
        match = created_async.search(line)
        if match and int(match[1]) in params:
            buffers[int(match[2])] = params[int(match[1])]
        match = attach.search(line)
        if match:
            attached = buffers.get(int(match[1])) if match[1] is not None else None
        if not recent:
            continue
        match = configured.search(line)
        if match:
            wanted = (int(match[1]), int(match[2])) == logical
            serial, acked = None, False
        match = serial_event.search(line)
        if match and wanted:
            serial = int(match[1])
        match = ack_event.search(line)
        if match and serial is not None and int(match[1]) == serial:
            acked = True
        if acked and commit.search(line) and attached is not None and attached[0] == expected:
            evidence = {"configure_serial": serial, "logical_size": logical,
                        "native_buffer_pixels": attached[0], "buffer_transport": attached[1]}
    return evidence


def stop_group(process):
    if process is None:
        return
    # Kill the owned group even if its leader exited, so helper children cannot leak.
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        pass
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait(timeout=2)


class HeadlessSway:
    """Own a private compositor; callers never borrow the desktop's display or IPC."""

    def __init__(self, runtime, artifacts, env=None, outputs=None, timeout=20):
        self.runtime, self.artifacts = Path(runtime), Path(artifacts)
        self.timeout = timeout
        self.outputs = outputs or (("HEADLESS-1", 1440, 960, 0, 0, 1),
                                   ("HEADLESS-2", 1800, 1200, 1440, 0, 1.25))
        self.env = {key: value for key, value in (env or os.environ).items()
                    if not key.startswith("WLR_") and key not in
                    {"DISPLAY", "WAYLAND_DISPLAY", "WAYLAND_SOCKET", "WAYLAND_DEBUG",
                     "SWAYSOCK", "I3SOCK", "XDG_SESSION_ID", "DBUS_SESSION_BUS_ADDRESS"}}
        self.process = self.log = None

    def alive(self):
        require(self.process.poll() is None,
                f"Sway exited {self.process.returncode}; see {self.artifacts / 'sway.log'}")

    def until(self, description, condition):
        deadline = time.monotonic() + self.timeout
        while True:
            self.alive()
            result = condition()
            if result:
                return result
            require(time.monotonic() < deadline, f"timed out waiting for {description}")
            time.sleep(0.025)

    def ipc(self, kind, message=None):
        self.alive()
        argv = ["swaymsg", "--socket", str(self.runtime / "sway.sock"), "--raw", "--type", kind]
        if message is not None:
            argv.append(message)
        result = subprocess.run(argv, env=self.env, capture_output=True, text=True,
                                timeout=min(self.timeout, 5))
        require(result.returncode == 0,
                f"Sway IPC failed: {result.stderr.strip()} {result.stdout.strip()}")
        reply = json.loads(result.stdout)
        if kind == "command":
            require(reply and all(item.get("success") for item in reply),
                    f"Sway command rejected: {message}: {reply}")
        return reply

    def __enter__(self):
        self.runtime.mkdir(mode=0o700, parents=True, exist_ok=True)
        require(stat.S_IMODE(self.runtime.stat().st_mode) == 0o700,
                "private XDG_RUNTIME_DIR is not mode 0700")
        self.artifacts.mkdir(parents=True, exist_ok=True)
        self.env.update(XDG_RUNTIME_DIR=str(self.runtime),
                        SWAYSOCK=str(self.runtime / "sway.sock"),
                        I3SOCK=str(self.runtime / "sway.sock"),
                        XDG_SESSION_TYPE="wayland", XDG_CURRENT_DESKTOP="sway",
                        XDG_SESSION_DESKTOP="sway", WLR_BACKENDS="headless",
                        WLR_HEADLESS_OUTPUTS=str(len(self.outputs)), WLR_RENDERER="pixman",
                        WLR_NO_HARDWARE_CURSORS="1",
                        DBUS_SESSION_BUS_ADDRESS=f"unix:path={self.runtime}/no-session-bus")
        config = self.artifacts / "sway.conf"
        config.write_text(
            "# Private session: no includes, bars, execs, Xwayland, or host outputs.\n"
            "swaybg_command -\nswaynag_command -\nxwayland disable\nfont monospace 10\n"
            "default_border none\ndefault_floating_border none\n"
            "floating_minimum_size 1 x 1\nfloating_maximum_size -1 x -1\n"
            "gaps inner 0\ngaps outer 0\nfocus_follows_mouse no\nmouse_warping none\n"
            "seat seat0 fallback true\n" +
            "".join(f"output {name} mode --custom {width}x{height}@60Hz position {x} {y} scale {scale}\n"
                    for name, width, height, x, y, scale in self.outputs) +
            "".join(f"workspace {index} output {output[0]}\n"
                    for index, output in enumerate(self.outputs, 1)) + "workspace 1\n")
        self.log = (self.artifacts / "sway.log").open("wb")
        try:
            self.process = subprocess.Popen(["sway", "--config", str(config), "--debug"],
                                            env=self.env, stdout=self.log, stderr=subprocess.STDOUT,
                                            start_new_session=True)
            self.until("owned Sway IPC socket", lambda: (self.runtime / "sway.sock").is_socket())
            sockets = self.until("private Wayland display",
                                 lambda: [path for path in self.runtime.glob("wayland-*") if path.is_socket()])
            require(len(sockets) == 1, f"expected one private Wayland socket, found {sockets}")
            self.env["WAYLAND_DISPLAY"] = sockets[0].name
            active = {item["name"] for item in self.ipc("get_outputs") if item["active"]}
            require(active == {item[0] for item in self.outputs},
                    f"unexpected outputs in private compositor: {active}")
            return self
        except BaseException:
            # Retain owned startup evidence before temporary artifacts are removed.
            try:
                self.log.flush()
                with (self.artifacts / "sway.log").open("rb") as log:
                    log.seek(0, os.SEEK_END)
                    log.seek(max(0, log.tell() - 4096))
                    tail = log.read(4096).decode("utf-8", errors="replace")
                print(f"--- owned Sway startup failure: {self.artifacts / 'sway.log'} "
                      "(last 4096 bytes) ---", file=sys.stderr)
                print(tail, file=sys.stderr, flush=True)
            except BaseException:
                pass  # Diagnostics must not replace the startup failure.
            try:
                self.__exit__(None, None, None)
            except BaseException:
                pass  # Preserve the startup failure even if cleanup also fails.
            raise

    def __exit__(self, *unused):
        stop_group(self.process)
        if self.log is not None:
            self.log.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output-dir", type=Path, required=True,
                        help="new artifact directory (must not already exist)")
    parser.add_argument("--timeout", type=float, default=20,
                        help="deadline in seconds for each native operation (default: 20)")
    parser.add_argument("--zoom", type=float, default=1,
                        help="user zoom in private test preferences (.7..2; default: 1)")
    parser.add_argument("command", nargs=argparse.REMAINDER,
                        help="-- CLIENT [ARGS], including a sufficient --frames limit if desired")
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command or not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("provide -- CLIENT [ARGS] and a positive finite --timeout")
    if not math.isfinite(args.zoom) or not .7 <= args.zoom <= 2:
        parser.error("--zoom must be finite and between .7 and 2")
    for binary in ("sway", "swaymsg", "grim", command[0]):
        if shutil.which(binary) is None:
            parser.error(f"{binary!r} is missing; invoke inside nix develop after fetching its dependencies")
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=False)
    original_env = os.environ.copy()
    report = {"status": "running", "command": command, "zoom": args.zoom, "stages": [],
              "limits": "Actual SHM/dma-buf dimensions and configure/ack/commit are measured; font crispness requires capture inspection. Unknown buffer transports fail, never substitute inferred dimensions."}
    sway = client = None
    phase = "initializing isolated compositor"
    logs = []
    error = None
    old_term = signal.getsignal(signal.SIGTERM)

    def interrupted(signum, frame):
        raise KeyboardInterrupt(f"received signal {signum}")

    signal.signal(signal.SIGTERM, interrupted)
    try:
        with ExitStack() as cleanup:
            temporary = cleanup.enter_context(tempfile.TemporaryDirectory(prefix="blastirc-dpi-"))
            runtime = Path(temporary)
            env = original_env.copy()
            for key, subdir in (("HOME", "home"), ("XDG_CONFIG_HOME", "config"),
                                ("XDG_DATA_HOME", "data"), ("XDG_STATE_HOME", "state"),
                                ("XDG_CACHE_HOME", "cache")):
                (runtime / subdir).mkdir(mode=0o700)
                env[key] = str(runtime / subdir)
            preferences = Path(env["XDG_CONFIG_HOME"]) / "blastirc"
            preferences.mkdir(mode=0o700)
            settings = preferences / "settings.toml"
            settings.write_text(
                f'[appearance]\nscale = {args.zoom}\n'
            )
            settings.chmod(0o600)
            sway = cleanup.enter_context(HeadlessSway(runtime / "wayland", out, env,
                                                    timeout=args.timeout))
            env = sway.env

            def alive():
                sway.alive()
                if client is not None:
                    require(client.poll() is None,
                            f"client exited with {client.returncode}; see client.stderr.log and client.stdout.log; increase its finite --frames limit if it ended normally")

            def until(description, condition):
                deadline = time.monotonic() + args.timeout
                while True:
                    alive()
                    result = condition()
                    if result:
                        return result
                    require(time.monotonic() < deadline,
                            f"timed out waiting for {description}; see protocol/client and compositor logs")
                    time.sleep(0.025)

            ipc_message = sway.ipc
            report["compositor"] = ipc_message("get_version")
            outputs = ipc_message("get_outputs")
            require({item["name"] for item in outputs if item["active"]} == {"HEADLESS-1", "HEADLESS-2"},
                    f"expected only two isolated headless outputs, found {outputs}")
            report["initial_outputs"] = outputs
            phase = "starting native Noctalia client"
            client_env = env | {"LIBGL_ALWAYS_SOFTWARE": "1", "WAYLAND_DEBUG": "client"}
            stdout = (out / "client.stdout.log").open("wb")
            stderr = (out / "client.stderr.log").open("wb")
            logs.extend((stdout, stderr))
            debug_path = out / "client.stderr.log"
            client = subprocess.Popen(command, env=client_env, stdout=stdout, stderr=stderr,
                                      start_new_session=True)
            # LIFO: stop the client, then Sway, then remove their private runtime.
            cleanup.callback(stop_group, client)

            def owned_window():
                windows = list(tree_windows(ipc_message("get_tree")))
                owned = []
                for node, output in windows:
                    try:
                        group = os.getpgid(node["pid"])
                    except ProcessLookupError:
                        continue
                    require(group == client.pid,
                            f"unexpected non-test client in private compositor: {node}")
                    owned.append((node, output))
                require(len(owned) <= 1, f"scenario requires one native window, found {owned}")
                return owned[0] if owned else None

            window, _ = until("one owned native Noctalia window", owned_window)
            require(window.get("shell") == "xdg_shell",
                    f"client is not native Wayland xdg-shell: {window}")
            require(window.get("app_id") == "io.github.trevarj.blastirc",
                    f"unexpected native application identity: {window}")
            identity = (window["id"], window["pid"])
            criterion = f"[con_id={identity[0]}]"
            ids = main_surface_ids(debug_path)
            report["window"] = {"id": identity[0], "pid": identity[1],
                                "title": window["name"], "app_id": window.get("app_id"),
                                "shell": window["shell"], "protocol_ids": ids}

            def current_window(output, size, fullscreen):
                found = owned_window()
                if found is None:
                    return None
                node, associated = found
                require((node["id"], node["pid"]) == identity,
                        "the original native window was replaced during the transition")
                rect = node["window_rect"]
                if (associated == output and node.get("visible")
                        and (rect["width"], rect["height"]) == size
                        and bool(node["fullscreen_mode"]) == fullscreen):
                    return node
                return None

            # A newly mapped window is already in the IPC tree while Sway still
            # awaits its initial tiled-size commit. Do not race that transaction
            # with a floating resize: an old client geometry can otherwise win.
            report["initial_response"] = until(
                "initial tiled configure/ack/commit at 1x",
                lambda: protocol_evidence(debug_path, ids, 0, LOGICAL, 1.0))
            until("initial committed window geometry",
                  lambda: current_window("HEADLESS-1", LOGICAL, False))

            for label, output, scale in STAGES:
                phase = f"{label}: moving/configuring the same running native window"
                pixels = tuple(round(value * scale) for value in LOGICAL)
                marker = debug_path.stat().st_size
                if output == "HEADLESS-2":
                    ipc_message("command", f"output {output} mode --custom {pixels[0]}x{pixels[1]}@60Hz scale {scale} position 1440 0")
                # Send placement and the real size change as one IPC transaction,
                # not two competing configure requests during a native resize.
                ipc_message("command", f"{criterion} fullscreen disable, floating enable, border none, move container to output {output}, resize set width {PROBE[0]} px height {PROBE[1]} px, focus")
                until(f"{label} probe geometry", lambda: current_window(output, PROBE, False))
                probe = until(f"{label} native resize ack, commit and {scale:g}x buffer",
                              lambda: protocol_evidence(debug_path, ids, marker, PROBE, scale))
                marker = debug_path.stat().st_size
                ipc_message("command", f"{criterion} fullscreen enable, focus")
                native = until(f"{label} fullscreen geometry",
                               lambda: current_window(output, LOGICAL, True))
                response = until(f"{label} fullscreen ack, commit and physical native buffer",
                                 lambda: protocol_evidence(debug_path, ids, marker, LOGICAL, scale))
                outputs = ipc_message("get_outputs")
                actual = next(item for item in outputs if item["name"] == output)
                require(math.isclose(actual["scale"], scale, abs_tol=1e-6),
                        f"{label}: compositor scale mismatch: {actual}")
                require((actual["rect"]["width"], actual["rect"]["height"]) == LOGICAL,
                        f"{label}: logical output dimensions mismatch: {actual}")
                require((actual["current_mode"]["width"], actual["current_mode"]["height"]) == pixels,
                        f"{label}: physical output mode mismatch: {actual}")
                # Give the compositor another refresh before screencopy, not just
                # the client's protocol commit. grim only sees this private session.
                time.sleep(0.1)
                alive()
                image = out / f"{label}.png"
                dimensions = capture_png(env, image, scale=actual["scale"],
                                         expected_pixels=pixels, output=output,
                                         timeout=args.timeout)
                require(current_window(output, LOGICAL, True) is not None,
                        f"{label}: original native window became unresponsive/unmapped during capture")
                stage = {"stage": label, "output": output, "scale": scale,
                         "logical_output_size": LOGICAL, "physical_capture_size": dimensions,
                         "capture": image.name, "probe": probe, "native_response": response,
                         "window": native, "outputs": outputs}
                report["stages"].append(stage)
                (out / f"{label}.json").write_text(json.dumps(stage, indent=2) + "\n")
            report["status"] = "passed"
    except (Exception, KeyboardInterrupt) as exc:
        error = f"{phase}: {type(exc).__name__}: {exc}"
        report.update(status="failed", error=error)
    finally:
        for stream in logs:
            stream.close()
        signal.signal(signal.SIGTERM, old_term)
        report["environment_unchanged"] = dict(os.environ) == original_env
        if not report["environment_unchanged"]:
            report.update(status="failed", error="driver environment unexpectedly changed")
            error = report["error"]
        (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    if error:
        print(error, file=sys.stderr)
        for name in ("sway.log", "client.stderr.log", "client.stdout.log"):
            path = out / name
            if path.exists():
                print(f"--- {name} (last 30 lines) ---", file=sys.stderr)
                print("\n".join(path.read_text(errors="replace").splitlines()[-30:]), file=sys.stderr)
        print(f"All diagnostics retained in {out}", file=sys.stderr)
        return 1
    print(f"PASS: same native Noctalia window, 1x -> 1.25x -> 1.5x -> 2x -> 1x; {out / 'report.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
