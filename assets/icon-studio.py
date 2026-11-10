import argparse
import io
import json
import re
import uuid
import warnings
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, quote, urlsplit

from PIL import Image, UnidentifiedImageError


ROOT = Path(__file__).resolve().parent.parent
ARTWORK = ROOT / "output" / "imagegen"
UPLOADS = ARTWORK / "studio"
Image.MAX_IMAGE_PIXELS = 20_000_000
warnings.simplefilter("error", Image.DecompressionBombWarning)


def variants():
    paths = [ROOT / "assets" / "icon.png"]
    if ARTWORK.is_dir():
        paths.extend(sorted(
            (p for p in ARTWORK.rglob("*")
             if p.suffix.lower() in {".png", ".jpg", ".jpeg", ".webp"}
             and p.is_file() and not p.is_symlink()
             and p.resolve().is_relative_to(ARTWORK.resolve())),
            key=lambda p: p.stat().st_mtime_ns, reverse=True,
        ))
    result = []
    for path in paths:
        try:
            with Image.open(path) as img:
                width, height = img.size
            relative = path.relative_to(ROOT).as_posix()
            result.append({
                "id": relative,
                "name": "Current app icon" if path == paths[0] else path.stem,
                "url": "/image/" + quote(relative, safe="/"),
                "width": width,
                "height": height,
                "revision": path.stat().st_mtime_ns,
            })
        except (OSError, UnidentifiedImageError, Image.DecompressionBombError,
                Image.DecompressionBombWarning):
            continue
    return result


class Handler(BaseHTTPRequestHandler):
    def allowed(self):
        hosts = {f"127.0.0.1:{self.server.server_port}",
                 f"localhost:{self.server.server_port}"}
        host = self.headers.get("Host", "")
        origin = self.headers.get("Origin")
        if host not in hosts or (origin and origin != f"http://{host}"):
            self.send_error(403)
            return False
        return True

    def respond(self, body, content_type, status=200):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Content-Security-Policy", "default-src 'self'; "
                         "script-src 'unsafe-inline'; style-src 'unsafe-inline'; "
                         "img-src 'self'; connect-src 'self'; frame-ancestors 'none'")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if not self.allowed():
            return
        route = urlsplit(self.path).path
        if route == "/":
            self.respond(Path(__file__).with_suffix(".html").read_bytes(),
                         "text/html; charset=utf-8")
        elif route == "/api/variants":
            self.respond(json.dumps(variants()).encode(), "application/json")
        elif route.startswith("/image/"):
            item = next((v for v in variants() if v["url"] == route), None)
            if item is None:
                self.send_error(404)
                return
            path = ROOT / item["id"]
            mime = {".png": "image/png", ".jpg": "image/jpeg",
                    ".jpeg": "image/jpeg", ".webp": "image/webp"}
            self.respond(path.read_bytes(), mime[path.suffix.lower()])
        else:
            self.send_error(404)

    def do_POST(self):
        if not self.allowed():
            return
        route = urlsplit(self.path)
        if route.path != "/api/variants":
            self.send_error(404)
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            self.send_error(400, "Invalid image length")
            return
        if not 0 < length <= 20 * 1024 * 1024:
            self.send_error(413, "Choose an image under 20 MB")
            return
        self.connection.settimeout(30)
        try:
            data = self.rfile.read(length)
            with Image.open(io.BytesIO(data)) as image:
                if image.format not in {"PNG", "JPEG", "WEBP"}:
                    raise ValueError("Unsupported image")
                image.load()
                normalized = io.BytesIO()
                image.convert("RGBA").save(normalized, format="PNG",
                                           icc_profile=image.info.get("icc_profile"))
        except (OSError, ValueError, Image.DecompressionBombError,
                Image.DecompressionBombWarning):
            self.send_error(400, "Choose a PNG, JPEG, or WebP up to 20 megapixels")
            return
        name = parse_qs(route.query).get("name", ["variant"])[0]
        stem = re.sub(r"[^a-zA-Z0-9_-]+", "-", Path(name).stem).strip("-")[:60]
        filename = f"{stem or 'variant'}-{uuid.uuid4().hex[:8]}.png"
        UPLOADS.mkdir(parents=True, exist_ok=True)
        destination = UPLOADS / filename
        temporary = destination.with_suffix(".tmp")
        try:
            temporary.write_bytes(normalized.getvalue())
            temporary.replace(destination)
        except OSError:
            temporary.unlink(missing_ok=True)
            self.send_error(500, "Could not save image")
            return
        self.respond(json.dumps({"id": destination.relative_to(ROOT).as_posix()}).encode(),
                     "application/json", 201)

    def log_message(self, format, *args):
        if args and str(args[1] if len(args) > 1 else "").startswith(("4", "5")):
            super().log_message(format, *args)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"Icon studio: http://localhost:{server.server_port}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
