#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["cryptography>=44,<47"]
# ///
"""Exercise real Dreamcast HTTPS against local TLS fixtures and a public cart."""
import argparse
from datetime import datetime, timedelta, timezone
import http.server
import ipaddress
import os
from pathlib import Path
import shutil
import socket
import ssl
import struct
import subprocess
import threading
import time
import zlib
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--host", help="Mac LAN IPv4 address reachable by Flycast's proxy")
parser.add_argument("--timeout", type=int, default=600)
parser.add_argument("--log", type=Path, default=ROOT / "build/network.log")
parser.add_argument("--no-public", action="store_true")
parser.add_argument("--modem", action="store_true", help="Use Flycast's modem PPP peer")
parser.add_argument("--screen-slow", action="store_true", help="Keep the download UI visible for inspection")
args = parser.parse_args()
if not args.host:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.connect(("1.1.1.1", 443))
        args.host = probe.getsockname()[0]
ipaddress.IPv4Address(args.host)
work = ROOT / "build/network-fixtures"
work.mkdir(parents=True, exist_ok=True)
now = datetime.now(timezone.utc)
ca_key = ec.generate_private_key(ec.SECP256R1())
ca_name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "PICO8 ephemeral test root")])
ca = (x509.CertificateBuilder().subject_name(ca_name).issuer_name(ca_name)
      .public_key(ca_key.public_key()).serial_number(x509.random_serial_number())
      .not_valid_before(now - timedelta(days=2)).not_valid_after(now + timedelta(days=2))
      .add_extension(x509.BasicConstraints(ca=True, path_length=0), critical=True)
      .sign(ca_key, hashes.SHA256()))

def certificate(name, *, expired=False, wrong_name=False, untrusted=False):
    key = ec.generate_private_key(ec.SECP256R1())
    subject = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, name)])
    cert = (x509.CertificateBuilder().subject_name(subject)
            .issuer_name(subject if untrusted else ca_name).public_key(key.public_key())
            .serial_number(x509.random_serial_number())
            .not_valid_before(now - timedelta(days=3))
            .not_valid_after(now - timedelta(days=1) if expired else now + timedelta(days=1))
            .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
            .add_extension(x509.SubjectAlternativeName([
                x509.DNSName("wrong.invalid") if wrong_name else x509.IPAddress(ipaddress.ip_address(args.host))
            ]), critical=False)
            .sign(key if untrusted else ca_key, hashes.SHA256()))
    cert_path, key_path = work / (name + ".pem"), work / (name + ".key")
    cert_path.write_bytes(cert.public_bytes(serialization.Encoding.PEM))
    key_path.write_bytes(key.private_bytes(serialization.Encoding.PEM,
                                          serialization.PrivateFormat.PKCS8,
                                          serialization.NoEncryption()))
    key_path.chmod(0o600)
    return cert_path, key_path

cart = (ROOT / "romdisk/tests/api.p8").read_bytes()
png = (ROOT / "romdisk/carts/picolumia.p8.png").read_bytes()
def corrupt_png(header):
    data = bytearray(160 * 205)
    data[0x4300:0x4300 + len(header)] = header
    raw = bytearray()
    for y in range(205):
        raw.append(0)
        for v in data[y * 160:(y + 1) * 160]:
            raw.extend(((v >> 4) & 3, (v >> 2) & 3, v & 3, (v >> 6) & 3))
    def chunk(kind, contents):
        return (struct.pack(">I", len(contents)) + kind + contents +
                struct.pack(">I", zlib.crc32(kind + contents)))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 160, 205, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
bad_pxa = corrupt_png(b"\0pxa\0\x10\xff\xff")
bad_legacy = corrupt_png(b":c:\0\0\x10\0\0\x3c\0")

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *_): pass
    def do_GET(self):
        try:
            self.serve()
        except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
            pass
    def serve(self):
        path = self.path.split("?")[0]
        if path in ("/redirect", "/downgrade", "/loop"):
            self.send_response(302)
            self.send_header("Location", {"/redirect": "folder/../chunked",
                "/downgrade": "http://example.com/cart.p8", "/loop": "/loop"}[path])
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if path == "/404":
            self.send_error(404)
            return
        self.send_response(200)
        if path == "/oversize":
            self.send_header("Content-Length", str(1024 * 1024 + 1))
            self.end_headers()
            return
        body = png if path == "/cart.png" else b"<html>not a cart</html>" if path == "/html" else cart
        if path == "/include":
            body = b"pico-8 cartridge\nversion 42\n__lua__\n#include /rd/launcher.p8\n"
        if path == "/badsfx":
            body = cart + b"\n__sfx__\n00\n"
        if path == "/badgfx":
            body = cart + b"\n__gfx__\n" + b"0" * 16386 + b"\n"
        if path == "/badpxa": body = bad_pxa
        if path == "/badlegacy": body = bad_legacy
        chunked = path in ("/chunked", "/badchunk")
        self.send_header("Transfer-Encoding" if chunked else "Content-Length",
                         "chunked" if chunked else str(len(body) + (30 if path == "/truncated" else 0)))
        self.send_header("Connection", "close")
        self.end_headers()
        if path == "/badchunk":
            self.wfile.write(b"zz\r\n")
        elif chunked:
            for offset in range(0, len(body), 37):
                chunk = body[offset:offset + 37]
                self.wfile.write(f"{len(chunk):x};test=yes\r\n".encode() + chunk + b"\r\n")
            self.wfile.write(b"0\r\nX-Test: done\r\n\r\n")
        elif path in ("/slow", "/screen-slow"):
            for offset in range(0, len(body), 64):
                self.wfile.write(body[offset:offset + 64]); self.wfile.flush()
                time.sleep(3 if path == "/screen-slow" else 0.25)
        else:
            self.wfile.write(body)
        self.close_connection = True

servers = []
def server(name, **opts):
    srv = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Handler)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(*certificate(name, **opts))
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    servers.append(srv)
    return f"https://{args.host}:{srv.server_port}"

base = server("valid")
bad = server("untrusted", untrusted=True)
expired = server("expired", expired=True)
wrong = server("wrong", wrong_name=True)
cases = [(name, expected, base + endpoint) for name, expected, endpoint in [
    ("TEXT", 1, "/cart.p8"), ("PNG", 1, "/cart.png"), ("REDIRECT_CHUNKED", 1, "/redirect"),
    ("HTTP_404", 0, "/404"), ("OVERSIZE", 0, "/oversize"),
    ("HTML", 0, "/html"), ("INCLUDE", 0, "/include"), ("TRUNCATED", 0, "/truncated"),
    ("BAD_SFX", 0, "/badsfx"), ("BAD_GFX", 0, "/badgfx"),
    ("BAD_PXA", 0, "/badpxa"), ("BAD_LEGACY", 0, "/badlegacy"),
    ("BAD_CHUNK", 0, "/badchunk"), ("DOWNGRADE", 0, "/downgrade"), ("LOOP", 0, "/loop"),
    ("CANCEL", 2, "/slow")]]
cases += [("UNTRUSTED", 0, bad + "/cart.p8"), ("EXPIRED", 0, expired + "/cart.p8"),
          ("WRONG_HOST", 0, wrong + "/cart.p8"), ("HTTP_URL", 0, "http://example.com/cart.p8"),
          ("USERINFO", 0, "https://name@example.com/cart.p8")]
if not args.no_public:
    cases += [("PUBLIC_GITHUB", 1,
        "https://raw.githubusercontent.com/derpycoder/bull-sheep/e34dd53d3ccb41a25156733bdf9e5e16061127b2/bull_sheep.p8")]
if args.modem:
    cases = [case for case in cases if case[0] in ("TEXT", "PNG", "PUBLIC_GITHUB")]
rom = ROOT / "build/network-romdisk"
shutil.copytree(ROOT / "romdisk", rom, dirs_exist_ok=True)
(rom / "certs/ca-bundle.pem").write_bytes((ROOT / "romdisk/certs/ca-bundle.pem").read_bytes()
                                        + ca.public_bytes(serialization.Encoding.PEM))
(rom / "tests/network-cases.txt").write_text("".join(f"{n} {e} {u}\n" for n, e, u in cases))
(rom / "tests/screen-url.txt").write_text(base + ("/screen-slow\n" if args.screen_slow else "/cart.p8\n"))
(ROOT / "build/network-romdisk.stamp").touch()
args.log.parent.mkdir(parents=True, exist_ok=True)
process = None
result = "FAIL: timed out"
try:
    subprocess.run(["make", "-C", str(ROOT), "-j8", "pico8-network-test.elf"], check=True)
    with args.log.open("w") as output:
        process = subprocess.Popen([str(ROOT / "run-flycast.sh"), "--skip-build", "--network-test"] +
                                   (["--modem"] if args.modem else []),
                                   stdout=output, stderr=subprocess.STDOUT)
        end = time.monotonic() + args.timeout
        while time.monotonic() < end:
            log = args.log.read_text(errors="replace")
            if "Kernel panic" in log or "Unhandled exception" in log:
                result = "FAIL: Dreamcast exception"; break
            if "PICO8: player exited cleanly status=" in log:
                expected = [f"PICO8_NET_TEST: PASS {n} " for n, _, _ in cases]
                expected += ["PICO8_NET_TEST: PASS LAUNCHER_INPUT_IDLE",
                             "PICO8_NET_TEST: PASS URL_SCREEN_LAUNCH", "PICO8_NET_TEST: PASS ALL",
                             "PICO8: player exited cleanly status=0"]
                result = "PASS" if all(s in log for s in expected) else "FAIL: see serial log"
                break
            if process.poll() is not None:
                result = "FAIL: Flycast exited early"; break
            time.sleep(0.25)
finally:
    if process is not None and process.poll() is None:
        process.terminate()
        try: process.wait(timeout=5)
        except subprocess.TimeoutExpired: process.kill(); process.wait()
    for srv in servers: srv.shutdown(); srv.server_close()
    # Private fixture keys have no use after this run.
    for key in work.glob("*.key"): key.unlink()
print(f"{result}\nSerial log: {args.log}")
raise SystemExit(0 if result == "PASS" else 1)
