#!/usr/bin/env python3
"""Real local Ergo/soju/ZNC fixtures; import LocalTopology for GUI verification.

Run from the Nix development shell, after building blastirc-network-probe:
  python3 tests/integration/run.py --probe build/blastirc-network-probe
The default idle window is 610 seconds. --idle-seconds explicitly shortens it.
No daemon, account, certificate, database or listener survives the context.
"""

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import secrets
import shutil
import signal
import socket
import ssl
import subprocess
import sys
import tempfile
import time


def secret_file(path, text):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(descriptor, "w") as stream:
        stream.write(text)
    os.chmod(path, 0o600)


class BootstrapClient:
    """A bootstrap/control connection to the genuine daemon, never a relay."""

    def __init__(self, endpoint, ca_file, authenticate=True):
        self.socket = ssl.create_default_context(cafile=str(ca_file)).wrap_socket(
            socket.create_connection((endpoint["host"], endpoint["port"]), timeout=10),
            server_hostname=endpoint["host"],
        )
        self.stream = self.socket.makefile("rb")
        try:
            if authenticate:
                self.send("CAP LS 302")
                available = set()
                while True:
                    _, params, _ = self.wait(lambda command, params, prefix: command == "CAP" and "LS" in params)
                    available.update(cap.split("=", 1)[0] for cap in params[-1].split())
                    if len(params) < 4 or params[-2] != "*":
                        break
                if "sasl" not in available:
                    raise RuntimeError("bootstrap endpoint does not advertise required SASL")
                requested = [cap for cap in ("sasl", "batch", "server-time", "echo-message", "message-tags") if cap in available]
                self.send("CAP REQ :" + " ".join(requested))
                self.wait(lambda command, params, prefix: command == "CAP" and "ACK" in params)
            self.send("NICK " + endpoint["nick"])
            self.send("USER " + endpoint["username"].split("/", 1)[0] + " 0 * :BlastIRC local fixture")
            if authenticate:
                self.send("AUTHENTICATE PLAIN")
                self.wait(lambda command, params, prefix: command == "AUTHENTICATE" and params == ["+"])
                payload = ("\0" + endpoint["username"] + "\0" + endpoint["password"]).encode()
                self.send("AUTHENTICATE " + base64.b64encode(payload).decode())
                self.wait(lambda command, params, prefix: command == "903")
                self.send("CAP END")
            self.wait(lambda command, params, prefix: command == "001")
        except BaseException:
            self.close()
            raise

    def send(self, line):
        if "\r" in line or "\n" in line:
            raise ValueError("framing bytes in fixture command")
        self.socket.sendall((line + "\r\n").encode())

    def read(self, deadline):
        while True:
            self.socket.settimeout(max(0.01, deadline - time.monotonic()))
            frame = self.stream.readline(16385)
            if not frame:
                raise RuntimeError("bootstrap daemon closed the IRC connection")
            if len(frame) > 16384 or not frame.endswith(b"\n"):
                raise RuntimeError("oversized bootstrap IRC frame")
            line = frame.decode("utf-8", errors="replace").rstrip("\r\n")
            if line.startswith("@"):
                line = line.split(" ", 1)[1]
            prefix = ""
            if line.startswith(":"):
                prefix, line = line[1:].split(" ", 1)
            head, separator, trailing = line.partition(" :")
            parts = head.split()
            if separator:
                parts.append(trailing)
            command, params = parts[0], parts[1:]
            if command == "PING":
                self.send("PONG :" + params[-1])
                continue
            if command == "ERROR" or command in {"464", "465", "904", "905", "906"}:
                raise RuntimeError("fixture registration rejected: " + command)
            return command, params, prefix

    def wait(self, predicate, timeout=30):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            item = self.read(deadline)
            if predicate(*item):
                return item
        raise TimeoutError("fixture IRC response deadline expired")

    def close(self):
        try:
            self.socket.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.stream.close()
        self.socket.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


class LocalTopology:
    """Owns private TLS listeners, throwaway credentials and foreground daemons.

    endpoints has direct, peer, soju and znc dictionaries suitable for profiles.
    Each contains host/port/tls/nick/username/password/sasl/bouncer/channels.
    Set SSL_CERT_FILE=lab.ca_file in every client process. `peer` is a separate
    Ergo account, and unauthenticated observers may also JOIN #blastirc.
    Readiness verifies the local CA and hostname; TLS errors stop immediately.
    """

    def __init__(self):
        self._temporary = None
        self._children = []
        self._reservations = {}
        self.endpoints = {}
        self.logs = {}
        self.ca_file = ""
        self._passwords = []

    def _redact(self, text):
        for password in self._passwords:
            text = text.replace(password, "<test-secret>")
        return text

    def _run(self, *args, input_text=None):
        try:
            result = subprocess.run(args, input=input_text, text=True, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, cwd=self.root, env=self.environment, timeout=30)
        except subprocess.TimeoutExpired:
            raise TimeoutError(Path(args[0]).name + " fixture command timed out") from None
        if result.returncode:
            raise RuntimeError(Path(args[0]).name + " failed:\n" + self._redact(result.stdout))
        return result.stdout

    def _spawn(self, name, args, environment=None):
        log = self.root / (name + ".log")
        descriptor = os.open(log, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(descriptor, "wb") as output:
            child = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.STDOUT,
                                     cwd=self.root, env=environment or self.environment, start_new_session=True)
        self.logs[name] = log
        self._children.append(child)
        return child

    def _port(self, name):
        reservation = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        reservation.bind(("127.0.0.1", 0))
        self._reservations[name] = reservation
        return reservation.getsockname()[1]

    def _release(self, name):
        self._reservations.pop(name).close()

    def _tls_ready(self, port, child):
        listener = Path(child.args[0]).name + " TLS listener localhost:" + str(port)
        deadline = time.monotonic() + 30
        context = ssl.create_default_context(cafile=self.ca_file)
        last_error = None
        while time.monotonic() < deadline:
            if child.poll() is not None:
                raise RuntimeError(listener + " exited before readiness (status " + str(child.returncode) + ")")
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=0.3) as connection:
                    with context.wrap_socket(connection, server_hostname="localhost"):
                        return
            except ssl.SSLError as error:
                raise RuntimeError(listener + " failed TLS verification/handshake: " + self._redact(str(error))) from error
            except OSError as error:
                last_error = error
                time.sleep(0.05)
        raise TimeoutError(listener + " did not become ready: " + self._redact(str(last_error))) from last_error

    def _certificate(self):
        self.ca_file = str(self.root / "ca.pem")
        self._run("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                  "-subj", "/CN=BlastIRC disposable test root", "-addext", "basicConstraints=critical,CA:TRUE",
                  "-addext", "keyUsage=critical,keyCertSign",
                  "-keyout", str(self.root / "ca.key"), "-out", self.ca_file)
        self._run("openssl", "req", "-new", "-newkey", "rsa:2048", "-nodes", "-subj", "/CN=localhost",
                  "-keyout", str(self.root / "server.key"), "-out", str(self.root / "server.csr"))
        extensions = self.root / "server.ext"
        secret_file(extensions, "basicConstraints=critical,CA:FALSE\nkeyUsage=digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\nsubjectAltName=DNS:localhost,IP:127.0.0.1\n")
        self._run("openssl", "x509", "-req", "-in", str(self.root / "server.csr"), "-CA", self.ca_file,
                  "-CAkey", str(self.root / "ca.key"), "-CAcreateserial", "-days", "1", "-extfile", str(extensions),
                  "-out", str(self.root / "server.pem"))
        # Match Python's default strict chain checks before starting any daemons.
        self._run("openssl", "verify", "-x509_strict", "-CAfile", self.ca_file,
                  "-purpose", "sslserver", "-verify_hostname", "localhost", str(self.root / "server.pem"))
        self._run("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                  "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1",
                  "-keyout", str(self.root / "untrusted.key"), "-out", str(self.root / "untrusted.pem"))
        for private in self.root.glob("*.key"):
            private.chmod(0o600)
        self.environment["SSL_CERT_FILE"] = self.ca_file

    def _endpoint(self, port, username, nick, bouncer="none", channels=None, password=None):
        password = password or secrets.token_hex(20)
        self._passwords.append(password)
        return dict(host="localhost", port=port, tls=True, username=username, password=password,
                    nick=nick, sasl="PLAIN", server_password="", bouncer=bouncer, channels=channels or ["#blastirc"])

    def _ergo(self, port, invalid_port):
        config = self.root / "ergo.yaml"
        secret_file(config, f'''network:
  name: BlastIRCFixture
server:
  name: irc.localhost
  listeners:
    "127.0.0.1:{port}":
      tls: {{cert: "{self.root}/server.pem", key: "{self.root}/server.key"}}
    "127.0.0.1:{invalid_port}":
      tls: {{cert: "{self.root}/untrusted.pem", key: "{self.root}/untrusted.key"}}
  lookup-hostnames: false
  ip-cloaking: {{enabled: false}}
  ip-limits: {{count: false, throttle: false}}
  max-sendq: 256k
  idle-timeouts: {{registration: 30s, ping: 90s, disconnect: 150s}}
accounts:
  authentication-enabled: true
  registration:
    enabled: true
    allow-before-connect: true
    email-verification: {{enabled: false}}
    throttling: {{enabled: false}}
    bcrypt-cost: 4
  login-throttling: {{enabled: false}}
  nick-reservation: {{enabled: false}}
  multiclient: {{enabled: true, allowed-by-default: true, always-on: "disabled"}}
channels:
  default-modes: +nt
history:
  enabled: true
  channel-length: 2048
  client-length: 256
  autoresize-window: 0
  autoreplay-on-join: 0
  chathistory-maxmessages: 1000
  znc-maxmessages: 1000
  restrictions: {{expire-time: 0, query-cutoff: none}}
  persistent: {{enabled: false}}
datastore:
  path: "{self.root}/ergo.db"
  autoupgrade: true
languages: {{enabled: false}}
limits:
  nicklen: 32
  identlen: 20
  channellen: 64
  awaylen: 390
  kicklen: 390
  topiclen: 390
  registration-messages: 1024
logging:
  - {{method: stderr, type: "* -userinput -useroutput", level: info}}
''')
        self._run("ergo", "initdb", "--conf", str(config))
        self._release("direct")
        self._release("invalid")
        child = self._spawn("ergo", ["ergo", "run", "--conf", str(config)])
        self._tls_ready(port, child)

    def _register(self, endpoint):
        with BootstrapClient(endpoint, self.ca_file, authenticate=False) as client:
            client.send("PRIVMSG NickServ :REGISTER " + endpoint["password"] + " *")
            client.wait(lambda command, params, prefix: command == "NOTICE" and
                        prefix.split("!", 1)[0].lower() == "nickserv" and params[-1] == "Account created")
        # A second genuine SASL connection makes registration success unambiguous.
        with BootstrapClient(endpoint, self.ca_file):
            pass

    def _soju(self, port, upstream):
        config = self.root / "soju.conf"
        secret_file(config, f'''hostname localhost
listen ircs://127.0.0.1:{port}
tls {self.root}/server.pem {self.root}/server.key
listen unix+admin://{self.root}/soju-admin.sock
db sqlite3 {self.root}/soju.db
message-store db
''')
        self._release("soju")
        child = self._spawn("soju", ["soju", "-config", str(config)])
        self._tls_ready(port, child)
        deadline = time.monotonic() + 15
        while not (self.root / "soju-admin.sock").exists():
            if child.poll() is not None or time.monotonic() >= deadline:
                raise RuntimeError("soju admin socket unavailable")
            time.sleep(0.05)
        endpoint = self.endpoints["soju"]
        control = ["sojuctl", "-config", str(config)]
        self._run(*control, "user", "create", "-username", "fixture", "-password", endpoint["password"])
        user = control + ["user", "run", "fixture"]
        self._run(*user, "network", "create", "-name", "local", "-addr", f'ircs://127.0.0.1:{upstream["port"]}',
                  "-nick", upstream["nick"], "-username", upstream["username"], "-enabled=false")
        self._run(*user, "sasl", "set-plain", "-network", "local", upstream["username"], upstream["password"])
        self._run(*user, "channel", "create", "#blastirc-soju/local")
        self._run(*user, "network", "update", "local", "-enabled=true")
        with BootstrapClient(endpoint, self.ca_file) as downstream:
            self._verify_upstream(downstream, upstream["nick"], "#blastirc-soju")

    def _znc(self, port, upstream):
        endpoint = self.endpoints["znc"]
        salt = secrets.token_hex(16)
        digest = hashlib.sha256((endpoint["password"] + salt).encode()).hexdigest()
        fingerprint = self._run("openssl", "x509", "-in", str(self.root / "server.pem"), "-noout", "-fingerprint", "-sha256").strip().split("=", 1)[1]
        directory = self.root / "znc"
        secret_file(directory / "configs" / "znc.conf", f'''Version = 1.10.3
SSLCertFile = {self.root}/server.pem
SSLKeyFile = {self.root}/server.key
LoadModule = corecaps
LoadModule = saslplainauth
<Listener fixture>
    Host = 127.0.0.1
    Port = {port}
    IPv4 = true
    IPv6 = false
    SSL = true
</Listener>
<User fixture>
    Pass = sha256#{digest}#{salt}#
    Nick = {upstream["nick"]}
    AltNick = {upstream["nick"]}_
    Ident = {upstream["username"]}
    RealName = BlastIRC fixture
    MultiClients = true
    NoTrafficTimeout = 180
    AutoClearChanBuffer = false
    AutoClearQueryBuffer = false
    ChanBufferSize = 500
    QueryBufferSize = 500
    <Network local>
        IRCConnectEnabled = false
        Server = 127.0.0.1 +{upstream["port"]}
        TrustAllCerts = false
        TrustPKI = false
        TrustedServerFingerprint = {fingerprint}
        LoadModule = sasl
        <Chan #blastirc-znc>
        </Chan>
    </Network>
</User>
''')
        self._release("znc")
        child = self._spawn("znc", ["znc", "--foreground", "--no-color", "--datadir", str(directory)])
        self._tls_ready(port, child)
        with BootstrapClient(endpoint, self.ca_file) as downstream:
            downstream.send("PRIVMSG *sasl :Set " + upstream["username"] + " " + upstream["password"])
            downstream.wait(lambda command, params, prefix: prefix.split("!", 1)[0] == "*sasl" and
                            params[-1] == "Username has been set to [" + upstream["username"] + "]")
            downstream.send("PRIVMSG *sasl :Mechanism PLAIN")
            downstream.wait(lambda command, params, prefix: prefix.split("!", 1)[0] == "*sasl" and
                            params[-1] == "Current mechanisms set: PLAIN")
            downstream.send("PRIVMSG *sasl :RequireAuth yes")
            downstream.wait(lambda command, params, prefix: prefix.split("!", 1)[0] == "*sasl" and
                            params[-1] == "We require SASL negotiation to connect")
            downstream.send("PRIVMSG *status :Connect")
            self._verify_upstream(downstream, upstream["nick"], "#blastirc-znc")

    def _verify_upstream(self, downstream, upstream_nick, channel):
        # Inspect the genuine server's membership, then require a message traversing
        # its actual upstream and downstream sockets. A bouncer's synthetic JOIN
        # alone is not sufficient readiness evidence.
        with BootstrapClient(self.endpoints["peer"], self.ca_file) as observer:
            observer.send("JOIN " + channel)
            observer.wait(lambda command, params, prefix: command == "JOIN" and params[0] == channel and prefix.split("!", 1)[0] == self.endpoints["peer"]["nick"])
            downstream.send("JOIN " + channel)
            observer.send("NAMES " + channel)
            observer.wait(lambda command, params, prefix:
                          (command == "353" and upstream_nick in [nick.lstrip("~&@%+").split("!", 1)[0] for nick in params[-1].split()]) or
                          (command == "JOIN" and params[0] == channel and prefix.split("!", 1)[0] == upstream_nick))
            text = "fixture-ready-" + secrets.token_hex(8)
            observer.send("PRIVMSG " + channel + " :" + text)
            downstream.wait(lambda command, params, prefix: command == "PRIVMSG" and params == [channel, text] and prefix.split("!", 1)[0] == self.endpoints["peer"]["nick"])

    def __enter__(self):
        for program in ("openssl", "ergo", "soju", "sojuctl", "znc"):
            if not shutil.which(program):
                raise RuntimeError(program + " unavailable; run inside `nix develop`")
        self._temporary = tempfile.TemporaryDirectory(prefix="blastirc-integration-")
        self.root = Path(self._temporary.name)
        self.root.chmod(0o700)
        self.environment = os.environ.copy()
        self.environment.update(HOME=str(self.root), XDG_CONFIG_HOME=str(self.root / "config"), XDG_DATA_HOME=str(self.root / "data"))
        try:
            ports = {name: self._port(name) for name in ("direct", "invalid", "soju", "znc")}
            self.invalid_port = ports["invalid"]
            self.endpoints = {
                "direct": self._endpoint(ports["direct"], "primary", "primary"),
                "peer": self._endpoint(ports["direct"], "peer", "peer"),
                "soju": self._endpoint(ports["soju"], "fixture/local", "soju", "soju", ["#blastirc-soju"]),
                "znc": self._endpoint(ports["znc"], "fixture/local", "znc", "znc", ["#blastirc-znc"]),
            }
            upstream_soju = self._endpoint(ports["direct"], "soju", "soju")
            upstream_znc = self._endpoint(ports["direct"], "znc", "znc")
            self._certificate()
            self._ergo(ports["direct"], ports["invalid"])
            for endpoint in (self.endpoints["direct"], self.endpoints["peer"], upstream_soju, upstream_znc):
                self._register(endpoint)
            self._soju(ports["soju"], upstream_soju)
            self._znc(ports["znc"], upstream_znc)
            return self
        except BaseException:
            self.diagnostics()
            self.close()
            raise

    def manifest(self):
        return dict(ca_file=self.ca_file, invalid_tls_port=self.invalid_port, endpoints=self.endpoints)

    def diagnostics(self):
        for name, path in self.logs.items():
            print("=== " + name + " (last 40 log lines) ===", file=sys.stderr)
            lines = path.read_text(errors="replace").splitlines()
            print(self._redact("\n".join(lines[-40:])), file=sys.stderr)

    def close(self):
        for reservation in self._reservations.values():
            reservation.close()
        self._reservations.clear()
        for child in reversed(self._children):
            if child.poll() is None:
                try:
                    os.killpg(child.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
        for child in reversed(self._children):
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(child.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                child.wait()
        self._children.clear()
        if self._temporary:
            self._temporary.cleanup()
            self._temporary = None

    def __exit__(self, exception_type, *_):
        if exception_type and exception_type is not KeyboardInterrupt:
            self.diagnostics()
        self.close()


def run_probes(lab, executable, idle_seconds, search=False):
    children = []
    for kind in (("soju",) if search else ("direct", "soju", "znc")):
        environment = lab.environment.copy()
        environment.update(BLAST_TEST_KIND=kind, BLAST_TEST_TOKEN=secrets.token_hex(12),
                           BLAST_TEST_CHANNEL="#blastirc-" + kind,
                           BLAST_TEST_IDLE_SECONDS=str(idle_seconds), BLAST_TEST_INVALID_PORT=str(lab.invalid_port))
        for prefix, endpoint in (("BLAST_TEST_", lab.endpoints[kind]), ("BLAST_TEST_PEER_", lab.endpoints["peer"])):
            for key in ("host", "port", "username", "password", "nick", "sasl", "server_password"):
                environment[prefix + key.upper()] = str(endpoint[key])
        environment["BLAST_TEST_PEER_NICK"] += "-" + kind
        if search:
            environment["BLAST_TEST_SEARCH"] = "1"
            print("Private Soju executable (package version metadata):",
                  Path(shutil.which("soju")).resolve(), flush=True)
        children.append((kind, lab._spawn("probe-" + kind, [str(executable)], environment)))
    deadline = time.monotonic() + idle_seconds + 180
    while any(child.poll() is None for _, child in children):
        if any(child.poll() not in (None, 0) for _, child in children):
            raise RuntimeError("network probe failed; all fixture processes will be terminated")
        if time.monotonic() > deadline:
            raise TimeoutError("network probes exceeded idle window plus protocol deadline")
        time.sleep(0.1)
    for kind, child in children:
        print("=== " + kind + " ===")
        print(lab._redact(lab.logs["probe-" + kind].read_text(errors="replace")), end="", flush=True)
        if child.returncode:
            raise RuntimeError(kind + " network probe failed")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--probe", type=Path, default=Path("build/blastirc-network-probe"))
    parser.add_argument("--search-probe", type=Path,
                        help="run the Core remote-only Soju search consumer using blastirc-application-check")
    parser.add_argument("--idle-seconds", type=int, default=610)
    parser.add_argument("--manifest", type=Path, help="write a mode-0600 endpoint manifest while fixtures are alive")
    parser.add_argument("--hold", action="store_true", help="keep fixtures alive for an external GUI until SIGINT/SIGTERM")
    arguments = parser.parse_args()
    if arguments.idle_seconds < 1:
        parser.error("--idle-seconds must be positive")
    selected_probe = arguments.search_probe or arguments.probe
    if not arguments.hold and not selected_probe.is_file():
        parser.error("build the selected probe target first, or use --hold")
    executable = selected_probe.resolve()

    def terminate(signum, frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, terminate)
    manifest_created = False
    try:
        with LocalTopology() as lab:
            if arguments.manifest:
                descriptor = os.open(arguments.manifest, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
                manifest_created = True
                with os.fdopen(descriptor, "w") as output:
                    json.dump(lab.manifest(), output, indent=2)
                    output.write("\n")
                print("Fixture manifest: " + str(arguments.manifest.resolve()), flush=True)
            if arguments.hold:
                if not arguments.manifest:
                    print(json.dumps(lab.manifest(), indent=2), flush=True)
                while True:
                    if any(child.poll() is not None for child in lab._children):
                        raise RuntimeError("fixture daemon exited while held")
                    time.sleep(0.2)
            else:
                run_probes(lab, executable, arguments.idle_seconds, bool(arguments.search_probe))
        return 0
    except KeyboardInterrupt:
        return 130
    except Exception as error:
        print("FAIL " + str(error), file=sys.stderr)
        return 1
    finally:
        if manifest_created:
            arguments.manifest.unlink(missing_ok=True)


if __name__ == "__main__":
    raise SystemExit(main())
