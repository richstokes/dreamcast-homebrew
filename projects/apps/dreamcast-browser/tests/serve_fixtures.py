#!/usr/bin/env python3
"""Small deterministic pages for exercising the browser in Flycast.

Run with Python's standard library; no packages or external websites needed.
The default listener is local-only. Use --bind 0.0.0.0 only when the emulator
needs the Mac's LAN address. --check validates the embedded fixture, then exits.
"""

import argparse
import base64
import html
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import random
import struct
import threading
import time
from urllib.parse import urlsplit
import zlib


# A 16x16 RGB orange/blue checkerboard, deliberately smaller than image limits.
PNG = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAABAAAAAQCAIAAACQkWg2AAAAJUlEQVR4nGP4HyUCRyIV"
    "/+EIlzjDINRAjCJk8cGoYRAG60iMBwAFKnwQ+hLbEgAAAABJRU5ErkJggg=="
)
SVG = b'<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16"></svg>'
# The browser refuses a larger download than this for one image.
IMAGE_LIMIT = 192 * 1024


def png(width, height, rows, level=9):
    """Encode 8-bit RGB rows (each width * 3 bytes) as a PNG file."""

    def chunk(kind, payload):
        body = kind + payload
        return struct.pack("!I", len(payload)) + body + struct.pack("!I", zlib.crc32(body))

    raw = b"".join(b"\x00" + row for row in rows)
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack("!2I5B", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, level))
            + chunk(b"IEND", b""))


def wide_png():
    """800x400 stripes: wider and taller than the page, so it must be scaled."""
    rows = []
    for y in range(400):
        shade = 64 + (y * 191) // 399
        rows.append(b"".join(
            bytes(((x // 50) % 2 * 255, shade, 255 - shade)) for x in range(800)))
    return png(800, 400, rows)


def big_png():
    """Incompressible noise, deliberately larger than the image download limit."""
    noise = random.Random(1998)
    rows = [bytes(noise.getrandbits(8) for _ in range(600 * 3)) for _ in range(300)]
    return png(600, 300, rows, level=1)


WIDE_PNG = wide_png()
BIG_PNG = big_png()


def page(title, content):
    return (
        '<!doctype html><html><head><meta charset="utf-8">'
        f"<title>{html.escape(title)}</title></head><body>{content}</body></html>"
    ).encode("utf-8")


def index_page():
    navigation = "".join(
        f'<li><a href="/target?nav={i}">Navigation clutter {i}</a></li>'
        for i in range(1, 33)
    )
    links = "".join(
        f'<li><a href="/target?link={i}">Article link {i:03d}</a></li>'
        for i in range(1, 121)
    )
    return page(
        "Dreamcast browser fixture",
        f"""
<header><h1>Fixture site header</h1>
<a href="#section">Skip to final section</a></header>
<nav><h2>Navigation clutter</h2><ul>{navigation}</ul></nav>
<main id="main"><article>
<h1>Readable fixture article</h1>
<p>This article should appear first in reader mode.</p>
<p>Inline spacing: before <strong>bold</strong> after; some<strong>thing</strong>;
<a href="/target">linked words</a> followed by plain words.</p>
<div><div><p>Nested blocks should not leave several empty lines.</p></div></div>
<p hidden>HIDDEN ATTRIBUTE SENTINEL</p>
<p style="display: none">DISPLAY NONE SENTINEL</p>
<p><a href="#section">Jump to final section</a> |
<a href="/slow">Slow page</a> | <a href="/image-page">Slow image page</a></p>
<h2>Image failure isolation</h2>
<img src="/unsupported.svg" alt="Unsupported SVG should be skipped">
<img src="/missing.jpg" alt="HEAD 405 must not stop the next image">
<img src="/pixel.png" alt="Orange and blue checkerboard should load">
<h2>Links beyond the old limit</h2><ol>{links}</ol>
<section id="section"><h2>Final section reached</h2>
<p>Local anchors should reach this text without another HTTP request.</p>
<a href="/target?last=1">Final link still works</a>
<form action="/target" method="get"><label>Local GET fixture:
<input name="q" value="dreamcast fixture"></label><input type="submit" value="Search"></form>
<form action="https://example.com/" method="get"><label>Cross-origin HTTPS GET:
<input name="q" value="public fixture"></label><input type="submit" value="Open example.com"></form>
</section></article></main>
<footer><p>Footer clutter should disappear in reader mode.</p></footer>
""",
    )


INDEX = index_page()
SLOW = page(
    "Slow response fixture",
    '<main><h1>Slow page completed</h1><p>While loading, edit the address, '
    'scroll the previous page, or cancel with Escape.</p>'
    + "<p>Slow response padding.</p>" * 128
    + '<p><a href="/">Return to fixture</a></p></main>',
)


GALLERY = page(
    "Image gallery fixture",
    '<main><h1>Image gallery fixture</h1>'
    '<img src="/wide.png" alt="0 wide stripes, scaled to fit">'
    '<img src="/chunked.png" alt="1 no declared length">'
    '<img src="/big.png" alt="2 too large, declared">'
    '<img src="/big-chunked.png" alt="3 too large, undeclared">'
    '<img src="data:image/gif;base64,R0lGOD" data-src="/pixel.png?lazy=1" alt="4 lazy">'
    '<img src="/unsupported.webp" srcset="/pixel.png?narrow=1 320w, /wide.png?chosen=1 800w"'
    ' alt="5 responsive">'
    '<picture><source type="image/webp" srcset="/unsupported.webp">'
    '<source type="image/png" srcset="/pixel.png?picture=1">'
    '<img src="/unsupported.webp" alt="6 picture"></picture>'
    '<table><tr><th>Console</th><th>Year</th></tr>'
    '<tr><td>Dreamcast</td><td>1998</td></tr></table></main>',
)


def check_png(data, expected_width, expected_height):
    assert data.startswith(b"\x89PNG\r\n\x1a\n")
    offset, compressed = 8, bytearray()
    width = height = 0
    while offset < len(data):
        length = struct.unpack_from("!I", data, offset)[0]
        kind = data[offset + 4 : offset + 8]
        payload = data[offset + 8 : offset + 8 + length]
        crc = struct.unpack_from("!I", data, offset + 8 + length)[0]
        assert crc == zlib.crc32(kind + payload) & 0xFFFFFFFF
        if kind == b"IHDR":
            width, height, depth, color, *_ = struct.unpack("!2I5B", payload)
            assert (width, height, depth, color) == (expected_width, expected_height, 8, 2)
        if kind == b"IDAT":
            compressed.extend(payload)
        offset += length + 12
    pixels = zlib.decompress(compressed)
    assert len(pixels) == height * (1 + width * 3)
    assert all(pixels[row * (1 + width * 3)] == 0 for row in range(height))


def validate_fixtures():
    """Check actual PNG chunks, CRCs and inflated pixel dimensions offline."""
    check_png(PNG, 16, 16)
    check_png(WIDE_PNG, 800, 400)
    check_png(BIG_PNG, 600, 300)
    assert len(WIDE_PNG) < IMAGE_LIMIT < len(BIG_PNG)
    assert INDEX.count(b"Article link ") == 120
    assert b'id="section"' in INDEX and b'href="#section"' in INDEX


class FixtureServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True


class Handler(BaseHTTPRequestHandler):
    server_version = "DreamcastFixture/2"
    # Connections stay open between requests, as they do on real sites.
    protocol_version = "HTTP/1.1"

    def setup(self):
        super().setup()
        self.connection.settimeout(15)
        with self.server.log_lock:
            self.server.connections += 1

    def count_request(self):
        with self.server.log_lock:
            self.server.requests += 1
            return self.server.connections, self.server.requests

    def log_message(self, format, *args):
        # Keep arbitrary request data out of logs; log_request has a fixed form.
        pass

    def log_request(self, code="-", size="-"):
        del size
        with self.server.log_lock:
            if self.server.log_count >= self.server.max_logs:
                return
            self.server.log_count += 1
            elapsed = time.monotonic() - self.server.started
            print(f"{elapsed:7.2f}s {self.command} {urlsplit(self.path).path[:100]!r} {code}", flush=True)
            if self.server.log_count == self.server.max_logs:
                print("Request log limit reached; further requests stay quiet.", flush=True)

    def send_body(self, body, content_type="text/html; charset=utf-8", status=200, slow=False):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        if self.command == "HEAD" or status in (301, 302, 303, 307, 308):
            return
        try:
            if slow:
                # The first bytes arrive immediately; completion takes --delay.
                for part in range(8):
                    if part:
                        time.sleep(self.server.delay / 7)
                    start, end = len(body) * part // 8, len(body) * (part + 1) // 8
                    self.wfile.write(body[start:end])
                    self.wfile.flush()
            else:
                self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError, TimeoutError):
            pass  # Canceling a request is an expected part of these fixtures.

    def send_chunked(self, body, content_type):
        """No Content-Length: the size is unknown until the body has arrived."""
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Transfer-Encoding", "chunked")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        try:
            for start in range(0, len(body), 4096):
                part = body[start : start + 4096]
                self.wfile.write(f"{len(part):x}\r\n".encode() + part + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
        except (BrokenPipeError, ConnectionResetError, TimeoutError):
            self.close_connection = True  # The browser stops reading at its limit.

    def redirect(self, status, location):
        self.send_response(status)
        self.send_header("Location", location)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        route = urlsplit(self.path).path
        connections, requests = self.count_request()
        if route == "/stats":
            # Counted before this request, so two readings can be compared.
            self.send_body(page(f"connections={connections} requests={requests - 1}",
                                "<h1>Fixture server counters</h1>"))
        elif route == "/landing":
            self.send_body(page("Landed after POST", "<main><h1>Landed after POST</h1>"
                                f"<p>Fetched with {self.command}.</p></main>"))
        elif route == "/gallery":
            self.send_body(GALLERY)
        elif route == "/wide.png":
            self.send_body(WIDE_PNG, "image/png")
        elif route == "/chunked.png":
            self.send_chunked(PNG, "image/png")
        elif route == "/big.png":
            self.send_body(BIG_PNG, "image/png")
        elif route == "/big-chunked.png":
            self.send_chunked(BIG_PNG, "image/png")
        elif route == "/unsupported.webp":
            self.send_body(b"RIFF\x00\x00\x00\x00WEBPVP8 ", "image/webp")
        elif route == "/":
            self.send_body(INDEX)
        elif route == "/target":
            self.send_body(page("Target reached", '<main><h1>Target reached</h1>'
                                '<p>The link or GET form worked.</p><a href="/">Back to fixture</a></main>'))
        elif route == "/slow":
            self.send_body(SLOW, slow=True)
        elif route == "/image-page":
            self.send_body(page("Slow image fixture", '<main><h1>Slow image fixture</h1>'
                                '<p>Load images, then type or cancel during the transfer.</p>'
                                '<img src="/slow-image.png" alt="Slow checkerboard">'
                                '<img src="/pixel.png" alt="Fast checkerboard afterwards">'
                                '<p><a href="/">Back to fixture</a></p></main>'))
        elif route in ("/pixel.png", "/slow-image.png"):
            self.send_body(PNG, "image/png", slow=route == "/slow-image.png")
        elif route == "/unsupported.svg":
            self.send_body(SVG, "image/svg+xml")
        elif route == "/missing.jpg":
            self.send_body(b"Intentional image error", "text/plain", 405 if self.command == "HEAD" else 404)
        else:
            self.send_body(page("Not found", "<h1>Not found</h1>"), status=404)

    def do_POST(self):
        route = urlsplit(self.path).path
        self.count_request()
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            length = -1
        if not 0 <= length <= 1024:
            self.close_connection = True
            self.send_body(b"Fixture POST limit is 1024 bytes", "text/plain", 413)
            return
        body = self.rfile.read(length)
        # Browsers fetch a 301, 302 or 303 with GET; 307 and 308 post again.
        if route in ("/post-redirect/301", "/post-redirect/302", "/post-redirect/303"):
            self.redirect(int(route[-3:]), f"/landing?from={route[-3:]}")
            return
        if route in ("/post-redirect/307", "/post-redirect/308"):
            self.redirect(int(route[-3:]), "/post")
            return
        if route == "/post-redirect/away":
            self.redirect(307, "http://elsewhere.invalid/post")
            return
        if route == "/post-redirect/loop":
            self.redirect(307, "/post-redirect/loop")
            return
        if route != "/post":
            self.send_body(b"POST only supported at /post", "text/plain", 404)
            return
        # Echo only the size, never field values, credentials, or request headers.
        self.send_body(page("POST received", f"<main><h1>POST received</h1>"
                            f"<p>Received {len(body)} bytes.</p></main>"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--delay", type=float, default=8, help="Slow response duration, 0-30 seconds")
    parser.add_argument("--duration", type=float, default=900, help="Stop after this many seconds (default: 900)")
    parser.add_argument("--max-logs", type=int, default=200)
    parser.add_argument("--check", action="store_true", help="Validate fixtures and exit without listening")
    args = parser.parse_args()
    validate_fixtures()
    if args.check:
        print(f"Fixtures valid: {len(PNG)}-byte PNG, {len(WIDE_PNG)}-byte wide PNG, "
              f"{len(BIG_PNG)}-byte oversized PNG, 120 article links, local anchors.")
        return
    if not 0 <= args.delay <= 30 or args.duration <= 0 or args.max_logs < 0:
        parser.error("Require delay 0-30, positive duration, and nonnegative max-logs")
    with FixtureServer((args.bind, args.port), Handler) as server:
        server.delay = args.delay
        server.max_logs = args.max_logs
        server.log_count = 0
        server.connections = 0
        server.requests = 0
        server.log_lock = threading.Lock()
        server.started = time.monotonic()
        stop = threading.Timer(args.duration, server.shutdown)
        stop.daemon = True
        stop.start()
        print(f"Fixture server: http://{args.bind}:{server.server_port}/ "
              f"(stops in {args.duration:g}s)", flush=True)
        try:
            server.serve_forever(poll_interval=0.2)
        except KeyboardInterrupt:
            pass
        finally:
            stop.cancel()


if __name__ == "__main__":
    main()
